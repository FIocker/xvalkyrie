#include "runtime/modules/console_shader_rdna.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <unordered_map>

// From the console GPU's machine code (AMD's RDNA instruction set, as LLVM's gfx1013 has it)
// to HLSL. The registers become variables of the shader, each instruction a statement on
// 32-bit patterns, and the shader is run for one lane of a wave: a mask of lanes is all
// ones or zero, and what the hardware does only for the lanes of EXEC is done if this
// lane's is set. Branches forward skip statements, branches back make loops.
//
// What a shader is given is in docs/research/console-shaders.md. The descriptors a shader
// loads are followed while it is translated, to the root signature's register they stand
// for; a shader that gets at one in a way that is not followed is given up on, as is one
// with an instruction that is not known here. `why` then names the first such thing.
namespace xv::runtime::modules::rdna {
namespace {
enum class F {
    sop2, sopk, sop1, sopc, sopp, smem, vop2, vop1, vopc, vop3, vop3p, vintrp, ds, flat, mubuf,
    mtbuf, mimg, exp, unknown
};
struct Op {
    F format;
    std::uint32_t op;
    const char *name;
};
// every instruction LLVM's disassembler names for this GPU (tools/rdna_ops.py)
constexpr Op ops[] = {
#include "runtime/modules/console_shader_rdna_ops.inc"
};
const char *name_of(F format, std::uint32_t op) {
    static const auto names = [] {
        std::unordered_map<std::uint32_t, const char *> table;
        for (const auto &known : ops)
            table.emplace(static_cast<std::uint32_t>(known.format) << 16 | known.op, known.name);
        return table;
    }();
    const auto found = names.find(static_cast<std::uint32_t>(format) << 16 | op);
    return found == names.end() ? nullptr : found->second;
}

struct Unsupported {
    std::string what;
};
[[noreturn]] void give_up(std::string what) { throw Unsupported{std::move(what)}; }

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t at) {
    std::uint32_t value = 0;
    if (at + 4 <= bytes.size())
        std::memcpy(&value, bytes.data() + at, 4);
    return value;
}

struct Range {
    std::uint32_t type, count, base, space, offset; // type: 0 SRV, 1 UAV, 2 CBV, 3 sampler
};
struct Parameter {
    std::uint32_t type = 0; // 0 table, 1 constants, 2 CBV, 3 SRV, 4 UAV
    std::uint32_t reg = 0, space = 0, count = 0;
    std::vector<Range> ranges;
};
bool read_root_signature(std::span<const std::uint8_t> data, std::vector<Parameter> &parameters) {
    const auto version = word(data, 0), count = word(data, 4), at = word(data, 8);
    if ((version != 1 && version != 2) || count > 64 || at + 12ull * count > data.size())
        return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        Parameter parameter;
        parameter.type = word(data, at + 12 * i);
        const auto offset = word(data, at + 12 * i + 8);
        if (parameter.type == 0) {
            const auto ranges = word(data, offset), start = word(data, offset + 4);
            const std::size_t size = version == 2 ? 24 : 20;
            if (ranges > 4096 || start + size * ranges > data.size())
                return false;
            std::uint32_t append = 0;
            for (std::uint32_t r = 0; r < ranges; ++r) {
                const auto entry = start + size * r;
                Range range{word(data, entry), word(data, entry + 4), word(data, entry + 8),
                            word(data, entry + 12), word(data, entry + size - 4)};
                if (range.offset == ~0u)
                    range.offset = append;
                append = range.count == ~0u ? range.offset : range.offset + range.count;
                parameter.ranges.push_back(range);
            }
        } else if (parameter.type == 1) {
            parameter.reg = word(data, offset);
            parameter.space = word(data, offset + 4);
            parameter.count = word(data, offset + 8);
        } else if (parameter.type <= 4) {
            parameter.reg = word(data, offset);
            parameter.space = word(data, offset + 4);
        } else {
            return false;
        }
        parameters.push_back(std::move(parameter));
    }
    return true;
}

// one instruction, with the fields of its format
struct Ins {
    F format = F::unknown;
    std::uint32_t op = 0;
    std::string_view name;
    std::uint32_t at = 0, words = 1; // where it is, in words from the first instruction
    std::uint32_t w0 = 0, w1 = 0, literal = 0;
    std::uint32_t extra[3] = {}; // more address registers of an image instruction
    // The word after an instruction whose first source is not a register but says that
    // parts of registers are used (sub_dword) or another lane's (lanes).
    enum class Form { plain, sub_dword, lanes } form = Form::plain;
    std::uint32_t modifier = 0;
};
bool is(const Ins &ins, std::string_view name) { return ins.name == name; }
bool starts(std::string_view text, std::string_view with) { return text.substr(0, with.size()) == with; }
bool ends(std::string_view text, std::string_view with) {
    return text.size() >= with.size() && text.substr(text.size() - with.size()) == with;
}

// what is known of a scalar register while translating, besides its value at run time
struct Known {
    // table: the address of a descriptor table; descriptor: a word of a descriptor loaded
    // from one; root: half of the address a root descriptor gives; code: an address in the
    // shader's own code
    enum Kind { nothing, constant, table, descriptor, root, code } kind = nothing;
    std::uint32_t value = 0; // the constant; the root parameter of the others
    std::uint32_t offset = 0, part = 0; // a descriptor: its byte offset in the table, and
                                        // which of its words this is; a root: which half
    int dynamic = -1;         // a descriptor whose offset is only known at run time: the
                              // variable that has it
    std::uint32_t origin = 0; // the instruction that loaded the descriptor
    std::uint32_t stride = 0; // the upper half of a root address made into a buffer
                              // descriptor: the stride given to it
};
struct Resource {
    std::uint32_t type = 0, reg = 0, space = 0; // type as a Range's
    bool root = false;        // a root descriptor: an address, not a view
    std::uint32_t stride = 0; // of a root buffer, if the shader says
    int dynamic = -1;         // one of an array: the variable with its place in the table
    std::uint32_t parameter = 0, origin = 0;
};
// What a first pass over a shader found out that changes how it is translated: the
// descriptors written through, which are unordered access views where the table allows both.
struct Facts {
    std::set<std::uint32_t> written;
};
struct Retry {};

class Translator {
  public:
    Translator(Stage stage, const Shader &shader, Facts &facts)
        : stage_(stage), shader_(shader), facts_(facts) {}

    std::string run() {
        if (!read_root_signature(shader_.root_signature, parameters_))
            give_up("root signature");
        read_file();
        decode();
        find_blocks();
        prepare_registers();
        for (std::size_t block = 0; block < blocks_.size(); ++block)
            emit_block(block);
        return finish();
    }

  private:
    // ---- the file ----
    void read_file() {
        const auto file = shader_.file;
        if (file.size() < 0x140 || std::memcmp(file.data(), "FSSM", 4))
            give_up("no code");
        const auto region = word(file, 0xc0), size = word(file, 0xc8);
        if (region > file.size() || size > file.size() - region || size < 0x2c)
            give_up("no code");
        for (int i = 0; i < 4; ++i)
            header_[i] = word(file, region + 4 * i);
        for (std::uint32_t at = region + 0x28; at + 4 <= region + size; at += 4)
            code_.push_back(word(file, at));
        // the records after the code: where the root parameters are
        std::size_t at = region + size;
        while (word(file, at) == 0xbf9f0000)
            at += 4;
        bool any = false;
        const auto root = [&](std::uint32_t reg, std::uint32_t parameter) {
            known_[reg] = {Known::root, parameter};
            known_[reg + 1] = {Known::root, parameter, 0, 1};
        };
        for (; at + 20 <= file.size() && word(file, at + 8) != ~0u; at += 20) {
            const auto kind = word(file, at);
            if (kind != 3 && kind != 1)
                continue;
            const auto where = word(file, at + 16), parameter = where >> 8, reg = where & 0xff;
            if (parameter >= parameters_.size() || reg >= 105)
                continue;
            if (kind == 3 && parameters_[parameter].type == 0)
                known_[reg] = {Known::table, parameter}, any = true;
            else if (kind == 1 && parameters_[parameter].type >= 2)
                root(reg, parameter), any = true;
        }
        if (!any) { // no records: the parameters in order, from the first user register
            std::uint32_t reg = stage_ == Stage::compute ? 4 : 8;
            for (std::uint32_t p = 0; p < parameters_.size() && reg < 100; ++p) {
                if (parameters_[p].type == 0) {
                    known_[reg++] = {Known::table, p};
                } else if (parameters_[p].type == 1) {
                    reg += parameters_[p].count;
                } else {
                    root(reg, p);
                    reg += 2;
                }
            }
        }
        // a pixel shader: which register of its input signature each attribute is
        if (stage_ == Stage::pixel)
            for (std::size_t entry = 0x130; entry + 8 <= 0x130 + 8 * 32 && file[entry]; entry += 8)
                attributes_[file[entry + 3]] = file[entry + 1];
    }

    // ---- decoding ----
    // What is decoded is what the first instruction leads to: constants a shader reads
    // through its program counter lie between and after its instructions, and are not code.
    void decode() {
        std::map<std::uint32_t, Ins> found;
        std::vector<std::uint32_t> work{0};
        while (!work.empty()) {
            auto at = work.back();
            work.pop_back();
            while (at < code_.size() && !found.count(at)) {
                const auto ins = decode_at(at);
                found.emplace(at, ins);
                if (ins.name == "s_endpgm" || ins.name == "s_code_end")
                    break;
                if (ins.name == "s_setpc_b64" || ins.name == "s_rfe_b64")
                    give_up("a jump to an address in a register");
                if (branch(ins)) {
                    const auto target = target_of(ins);
                    if (target >= code_.size())
                        give_up("a branch out of the code");
                    work.push_back(target);
                    if (ins.name == "s_branch")
                        break;
                }
                at += ins.words;
            }
        }
        for (const auto &[at, ins] : found) {
            if (!code_list_.empty() && code_list_.back().at + code_list_.back().words > at)
                give_up("branch into the middle of an instruction");
            index_of_[at] = code_list_.size();
            code_list_.push_back(ins);
        }
        if (code_list_.empty())
            give_up("no code");
        data_first_ = code_list_.back().at + code_list_.back().words;
        find_modes();
    }
    Ins decode_at(std::uint32_t at) {
        Ins ins;
        ins.at = at;
        const auto w = ins.w0 = code_[at];
        const auto top = w >> 26;
        const auto second = [&] {
            if (at + 1 >= code_.size())
                give_up("truncated code");
            ins.w1 = code_[at + 1];
            ins.words = 2;
        };
        if (top == 0x33) ins.format = F::vop3p, ins.op = (w >> 16) & 0x7f, second();
        else if (top == 0x35) ins.format = F::vop3, ins.op = (w >> 16) & 0x3ff, second();
        else if (top == 0x32) ins.format = F::vintrp, ins.op = (w >> 16) & 3;
        else if (top == 0x36) ins.format = F::ds, ins.op = (w >> 18) & 0xff, second();
        else if (top == 0x37) ins.format = F::flat, ins.op = (w >> 18) & 0x7f, second();
        else if (top == 0x38) ins.format = F::mubuf, ins.op = (w >> 18) & 0x7f, second();
        else if (top == 0x3a) second(), ins.format = F::mtbuf, ins.op = ((w >> 16) & 7) | ((ins.w1 >> 21) & 1) << 3;
        else if (top == 0x3c) ins.format = F::mimg, ins.op = (w & 1) << 7 | ((w >> 18) & 0x7f), second();
        else if (top == 0x3d) ins.format = F::smem, ins.op = (w >> 18) & 0xff, second();
        else if (top == 0x3e) ins.format = F::exp, ins.op = 0, second();
        else if ((w >> 23) == 0x17d) ins.format = F::sop1, ins.op = (w >> 8) & 0xff;
        else if ((w >> 23) == 0x17e) ins.format = F::sopc, ins.op = (w >> 16) & 0x7f;
        else if ((w >> 23) == 0x17f) ins.format = F::sopp, ins.op = (w >> 16) & 0x7f;
        else if ((w >> 28) == 0xb) ins.format = F::sopk, ins.op = (w >> 23) & 0x1f;
        else if ((w >> 30) == 2) ins.format = F::sop2, ins.op = (w >> 23) & 0x7f;
        else if ((w >> 25) == 0x3f) ins.format = F::vop1, ins.op = (w >> 9) & 0xff;
        else if ((w >> 25) == 0x3e) ins.format = F::vopc, ins.op = (w >> 17) & 0xff;
        else if ((w >> 31) == 0) ins.format = F::vop2, ins.op = (w >> 25) & 0x3f;
        const auto *const name = name_of(ins.format, ins.op);
        if (!name) {
            char text[48];
            std::snprintf(text, sizeof text, "instruction %08x", w);
            give_up(text);
        }
        ins.name = name;
        const auto more = [&] {
            if (at + ins.words >= code_.size())
                give_up("truncated code");
            return code_[at + ins.words++];
        };
        // more words: the registers of an image instruction's address, or a literal
        if (ins.format == F::mimg)
            for (std::uint32_t n = 0, count = (w >> 1) & 3; n < count; ++n)
                ins.extra[n] = more();
        if (ins.format == F::vop1 || ins.format == F::vop2 || ins.format == F::vopc) {
            const auto source = w & 0x1ff;
            if (source == 249)
                ins.form = Ins::Form::sub_dword, ins.modifier = more();
            else if (source == 250 || source == 233 || source == 234)
                ins.form = Ins::Form::lanes, ins.modifier = more();
        }
        bool literal = false;
        const auto uses = [&](std::uint32_t source) { literal |= source == 255; };
        switch (ins.format) {
        case F::sop2: uses(w & 0xff), uses((w >> 8) & 0xff); break;
        case F::sop1: case F::sopc: uses(w & 0xff); if (ins.format == F::sopc) uses((w >> 8) & 0xff); break;
        case F::vop2: uses(w & 0x1ff); literal |= ins.name == "v_fmaak_f32" || ins.name == "v_fmamk_f32" || ins.name == "v_madak_f32" || ins.name == "v_madmk_f32" || ins.name == "v_fmaak_f16" || ins.name == "v_fmamk_f16"; break;
        case F::vop1: case F::vopc: uses(w & 0x1ff); break;
        case F::vop3: case F::vop3p:
            uses(ins.w1 & 0x1ff), uses((ins.w1 >> 9) & 0x1ff), uses((ins.w1 >> 18) & 0x1ff);
            break;
        case F::sopk: literal = ins.name == "s_setreg_imm32_b32"; break;
        default: break;
        }
        if (literal)
            ins.literal = more();
        return ins;
    }
    void find_modes() {
        for (const auto &ins : code_list_) {
            if (ins.format == F::sopp || ins.format == F::sop1 || ins.format == F::sop2) {
                const auto destination = (ins.w0 >> 16) & 0x7f;
                if ((ins.format != F::sopp && (destination == 126 || destination == 127)) ||
                    ins.name.find("saveexec") != std::string_view::npos)
                    exec_changes_ = true;
            }
            if (starts(ins.name, "v_cmpx"))
                exec_changes_ = true;
            if (ends(ins.name, "_b64") || ends(ins.name, "saveexec_b64"))
                wave64_ = true;
        }
    }

    // ---- control flow ----
    struct Block {
        std::size_t first, end; // instructions [first, end)
        bool guarded = false;   // a branch forward may jump over it
        int opens = 0;          // loops that start with it
        std::vector<std::size_t> closes; // back edges that end loops here are in `back`
    };
    std::size_t index_at(std::uint32_t address) {
        const auto found = index_of_.find(address);
        if (found == index_of_.end())
            give_up("branch into the middle of an instruction");
        return found->second;
    }
    bool branch(const Ins &ins) {
        return ins.format == F::sopp && (ins.name == "s_branch" || starts(ins.name, "s_cbranch"));
    }
    std::uint32_t target_of(const Ins &ins) {
        return ins.at + 1 + static_cast<std::int16_t>(ins.w0 & 0xffff);
    }
    void find_blocks() {
        std::set<std::size_t> leaders{0};
        for (std::size_t i = 0; i < code_list_.size(); ++i)
            if (branch(code_list_[i])) {
                leaders.insert(index_at(target_of(code_list_[i])));
                leaders.insert(i + 1);
            } else if (code_list_[i].name == "s_endpgm") {
                leaders.insert(i + 1);
            }
        leaders.erase(code_list_.size());
        for (auto it = leaders.begin(); it != leaders.end(); ++it) {
            const auto next = std::next(it);
            blocks_.push_back({*it, next == leaders.end() ? code_list_.size() : *next});
        }
        const auto block_of = [&](std::size_t instruction) {
            for (std::size_t b = 0; b < blocks_.size(); ++b)
                if (instruction >= blocks_[b].first && instruction < blocks_[b].end)
                    return b;
            return blocks_.size();
        };
        target_block_.assign(blocks_.size(), ~std::size_t{0});
        for (std::size_t b = 0; b < blocks_.size(); ++b) {
            const auto &last = code_list_[blocks_[b].end - 1];
            if (last.name == "s_endpgm") { // the end, from anywhere: over everything after
                for (std::size_t over = b + 1; over < blocks_.size(); ++over)
                    blocks_[over].guarded = true;
                continue;
            }
            if (!branch(last))
                continue;
            const auto to = block_of(index_at(target_of(last)));
            target_block_[b] = to;
            if (to > b) {
                for (std::size_t over = b + 1; over < to && over < blocks_.size(); ++over)
                    blocks_[over].guarded = true;
            } else {
                ++blocks_[to].opens;
                loop_end_[to] = std::max(loop_end_[to], b);
            }
        }
        // one loop for each header, up to its last back edge
        for (auto &[header, end] : loop_end_)
            blocks_[header].opens = 1;
    }

    // ---- registers ----
    std::string s(std::uint32_t reg) {
        used_s_.insert(reg);
        return "s" + std::to_string(reg);
    }
    std::string v(std::uint32_t reg) {
        used_v_.insert(reg & 0xff);
        return "v" + std::to_string(reg & 0xff);
    }
    static std::string hex(std::uint32_t value) {
        char text[16];
        std::snprintf(text, sizeof text, "0x%08xu", value);
        return text;
    }
    static std::string f(const std::string &bits) { return "asfloat(" + bits + ")"; }
    static std::string u(const std::string &value) { return "asuint(" + value + ")"; }
    // the constant an operand code stands for, if it is one
    static std::optional<std::uint32_t> constant_of(std::uint32_t source, std::uint32_t literal) {
        if (source >= 128 && source <= 192)
            return source - 128;
        if (source >= 193 && source <= 208)
            return static_cast<std::uint32_t>(-static_cast<std::int32_t>(source - 192));
        static constexpr float floats[] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f};
        if (source >= 240 && source <= 247)
            return std::bit_cast<std::uint32_t>(floats[source - 240]);
        if (source == 248)
            return 0x3e22f983; // 1 / (2 pi)
        if (source == 255)
            return literal;
        return std::nullopt;
    }
    // an operand: a scalar register, a constant, or (from 256) a vector register
    std::string operand(const Ins &ins, std::uint32_t source) {
        if (source >= 256)
            return v(source - 256);
        if (const auto value = constant_of(source, ins.literal))
            return hex(*value);
        if (source == 253)
            return "(scc ? 1u : 0u)";
        if (source == 251)
            return "((" + s(106) + " | " + s(107) + ") == 0u ? 1u : 0u)";
        if (source == 252)
            return "((" + s(126) + " | " + s(127) + ") == 0u ? 1u : 0u)";
        if (source == 125)
            return "0u";
        if (source < 128) {
            if (known_[source].kind == Known::table || known_[source].kind == Known::descriptor)
                give_up("a descriptor used as a value (" + std::string(ins.name) + ")");
            // an address of the console's has no meaning here, and only goes into the
            // descriptor a shader makes of it
            if (known_[source].kind == Known::root)
                return "0u";
            return s(source);
        }
        give_up("operand " + std::to_string(source));
    }
    // the upper half of a 64-bit operand
    std::string operand_high(const Ins &ins, std::uint32_t source) {
        if (const auto value = constant_of(source, ins.literal))
            return source >= 193 && source <= 208 ? "0xffffffffu" : "0u";
        if (source < 127)
            return operand(ins, source + 1);
        give_up("64-bit operand " + std::to_string(source));
    }
    void forget(std::uint32_t reg) {
        if (reg < 128)
            known_[reg] = {};
    }
    std::string active() { return "(" + s(126) + " != 0u)"; }
    void line(const std::string &text) { body_ += indent_ + text + "\n"; }
    // a write to a vector register, which only the lanes of EXEC do
    void set_v(std::uint32_t reg, const std::string &whole) {
        const auto value = placed_ ? place(reg, whole) : whole;
        if (exec_changes_)
            line("if " + active() + " " + v(reg) + " = " + value + ";");
        else
            line(v(reg) + " = " + value + ";");
    }
    void set_s(std::uint32_t reg, const std::string &value) {
        if (reg == 125)
            return;
        forget(reg);
        line(s(reg) + " = " + value + ";");
    }
    void set_mask(std::uint32_t reg, const std::string &condition) {
        const auto value = "((" + condition + ")" + (exec_changes_ ? " && " + active() : "") +
                           " ? 0xffffffffu : 0u)";
        set_s(reg, value);
        if (wave64_ && reg < 127) {
            forget(reg + 1);
            line(s(reg + 1) + " = " + s(reg) + ";");
        }
    }

    void prepare_registers() {
        used_s_.insert({106, 107, 126, 127});
        if (stage_ == Stage::pixel) {
            // the position's components that are read follow the barycentrics
            std::uint32_t reg = 2;
            for (std::size_t i = 0; i < shader_.inputs.size(); ++i) {
                const auto &element = shader_.inputs[i];
                if (_stricmp(element.name.c_str(), "SV_Position"))
                    continue;
                for (int c = 0; c < 4; ++c)
                    if (element.read >> c & 1)
                        prologue_ += "    " + v(reg++) + " = asuint(input.f" + std::to_string(i) +
                                     "." + "xyzw"[c] + ");\n";
            }
        } else if (stage_ == Stage::compute) {
            // after the user registers, the thread group's id as enabled; the thread's id
            // within its group in the first vector registers
            std::uint32_t reg = (header_[0] >> 1) & 0x1f;
            for (int c = 0; c < 3; ++c)
                if (header_[0] >> (7 + c) & 1)
                    prologue_ += "    " + s(reg++) + " = group." + "xyz"[c] + ";\n";
            for (int c = 0; c < 3; ++c)
                prologue_ += "    " + v(c) + " = thread." + "xyz"[c] + ";\n";
        }
    }

    // ---- emitting ----
    void emit_block(std::size_t index) {
        auto &block = blocks_[index];
        if (block.opens) {
            line("for (uint loop" + std::to_string(index) + " = 0u; loop" + std::to_string(index) +
                 " < 4096u; ++loop" + std::to_string(index) + ") {");
            indent_ += "    ";
            open_loops_.push_back(index);
        }
        if (block.guarded) {
            line("if (next <= " + std::to_string(index) + "u) {");
            indent_ += "    ";
        }
        for (std::size_t i = block.first; i < block.end; ++i) {
            const auto &ins = code_list_[i];
            if (i + 1 == block.end && (branch(ins) || ins.name == "s_endpgm"))
                break;
            emit(ins);
        }
        const auto &last = code_list_[block.end - 1];
        const auto to = target_block_[index];
        std::string leave; // what ends the loop that closes here, if it is not taken again
        if (last.name == "s_endpgm") {
            line("next = " + std::to_string(blocks_.size()) + "u;");
        } else if (branch(last)) {
            const auto condition = branch_condition(last);
            if (to > index)
                line((condition.empty() ? "" : "if (" + condition + ") ") + "next = " +
                     std::to_string(to) + "u;");
            else if (!open_loops_.empty() && loop_end_[to] == index && open_loops_.back() == to)
                leave = condition.empty() ? "" : "if (!(" + condition + ")) break;";
            else // a back edge that is not its loop's last: once more round
                line((condition.empty() ? "" : "if (" + condition + ") ") + "{ next = " +
                     std::to_string(to) + "u; continue; }");
        }
        if (block.guarded) {
            indent_.resize(indent_.size() - 4);
            line("}");
        }
        if (!open_loops_.empty() && loop_end_[open_loops_.back()] == index) {
            const auto header = open_loops_.back();
            // out of the loop if a branch went past its end, or its back edge is not taken
            line("if (next > " + std::to_string(index) + "u) break;");
            if (target_block_[index] == header && code_list_[blocks_[index].end - 1].name != "s_endpgm") {
                if (!leave.empty())
                    line(leave);
                // round again: what a branch forward skipped is not skipped the next time
                line("next = " + std::to_string(header) + "u;");
            } else {
                line("break;");
            }
            indent_.resize(indent_.size() - 4);
            line("}");
            open_loops_.pop_back();
        }
    }
    std::string branch_condition(const Ins &ins) {
        if (ins.name == "s_branch") return {};
        if (ins.name == "s_cbranch_scc0") return "!scc";
        if (ins.name == "s_cbranch_scc1") return "scc";
        if (ins.name == "s_cbranch_vccz") return "(" + s(106) + " | " + s(107) + ") == 0u";
        if (ins.name == "s_cbranch_vccnz") return "(" + s(106) + " | " + s(107) + ") != 0u";
        if (ins.name == "s_cbranch_execz") return "(" + s(126) + " | " + s(127) + ") == 0u";
        if (ins.name == "s_cbranch_execnz") return "(" + s(126) + " | " + s(127) + ") != 0u";
        give_up(std::string(ins.name));
    }

    void emit(const Ins &ins) {
        switch (ins.format) {
        case F::sopp: emit_sopp(ins); break;
        case F::sop1: emit_sop1(ins); break;
        case F::sop2: emit_sop2(ins); break;
        case F::sopk: emit_sopk(ins); break;
        case F::sopc: emit_sopc(ins); break;
        case F::smem: emit_smem(ins); break;
        case F::vop1: case F::vop2: case F::vopc: emit_short(ins); break;
        case F::ds: emit_shared(ins); break;
        case F::vop3: emit_vop3(ins); break;
        case F::vop3p: emit_vop3p(ins); break;
        case F::vintrp: emit_interpolation(ins); break;
        case F::mimg: emit_image(ins); break;
        case F::mubuf: emit_buffer(ins); break;
        case F::exp: emit_export(ins); break;
        default: give_up(std::string(ins.name));
        }
    }

    void emit_sopp(const Ins &ins) {
        if (ins.name == "s_nop" || ins.name == "s_waitcnt" || ins.name == "s_inst_prefetch" ||
            ins.name == "s_code_end" || ins.name == "s_barrier" || ins.name == "s_sendmsg")
            return;
        give_up(std::string(ins.name));
    }
    void emit_sop1(const Ins &ins) {
        const auto destination = (ins.w0 >> 16) & 0x7f, source = ins.w0 & 0xff;
        const auto name = ins.name;
        if (name == "s_swappc_b64") { // the call to the fetch shader
            fetch_inputs();
            return;
        }
        if (name == "s_mov_b32") {
            if (source < 128 && (known_[source].kind == Known::table || known_[source].kind == Known::descriptor ||
                                 known_[source].kind == Known::root)) {
                const auto copy = known_[source];
                known_[destination] = copy;
                return;
            }
            const auto value = constant_of(source, ins.literal);
            const bool code = source < 128 && known_[source].kind == Known::code;
            set_s(destination, operand(ins, source));
            if (value)
                known_[destination] = {Known::constant, *value};
            else if (code)
                known_[destination] = {Known::code};
            return;
        }
        if (name == "s_getpc_b64") { // where the next instruction is, from the first
            set_s(destination, hex(4 * (ins.at + 1)));
            set_s(destination + 1, "0u");
            known_[destination] = {Known::code};
            return;
        }
        if (name == "s_bitset0_b32" || name == "s_bitset1_b32") {
            const auto bit = "(1u << (" + operand(ins, source) + " & 31u))";
            const auto old = s(destination);
            set_s(destination, name == "s_bitset1_b32" ? old + " | " + bit : old + " & ~" + bit);
            return;
        }
        if (name == "s_bitset0_b64" || name == "s_bitset1_b64") {
            const bool set = name == "s_bitset1_b64";
            line("{ uint bit = " + operand(ins, source) + " & 63u, lo = bit < 32u ? 1u << bit : 0u, hi = bit < 32u ? 0u : 1u << (bit - 32u);");
            const auto low = s(destination), high = s(destination + 1);
            set_s(destination, set ? low + " | lo" : low + " & ~lo");
            set_s(destination + 1, set ? high + " | hi" : high + " & ~hi");
            line("}");
            return;
        }
        if (name == "s_mov_b64" || name == "s_wqm_b64" || name == "s_not_b64") {
            const auto low = operand(ins, source), high = operand_high(ins, source);
            const bool inverted = name == "s_not_b64";
            line("{ uint lo = " + std::string(inverted ? "~" : "") + low + ", hi = " + (inverted ? "~" : "") + high + ";");
            set_s(destination, "lo");
            set_s(destination + 1, "hi");
            if (name != "s_mov_b64")
                line("scc = (lo | hi) != 0u;");
            line("}");
            return;
        }
        if (ends(name, "saveexec_b64") || ends(name, "saveexec_b32")) {
            // the destination keeps EXEC, and EXEC becomes it combined with the source
            const bool wide = ends(name, "_b64");
            const auto combine = [&](const std::string &a, const std::string &exec) -> std::string {
                if (starts(name, "s_and_saveexec")) return a + " & " + exec;
                if (starts(name, "s_or_saveexec")) return a + " | " + exec;
                if (starts(name, "s_xor_saveexec")) return a + " ^ " + exec;
                if (starts(name, "s_andn2_saveexec")) return a + " & ~" + exec;
                if (starts(name, "s_andn1_saveexec")) return "~" + a + " & " + exec;
                if (starts(name, "s_orn2_saveexec")) return a + " | ~" + exec;
                if (starts(name, "s_nand_saveexec")) return "~(" + a + " & " + exec + ")";
                if (starts(name, "s_nor_saveexec")) return "~(" + a + " | " + exec + ")";
                give_up(std::string(name));
            };
            line("{ uint lo = " + combine(operand(ins, source), s(126)) + ", hi = " +
                 (wide ? combine(operand_high(ins, source), s(127)) : std::string("0u")) + ";");
            set_s(destination, s(126));
            if (wide)
                set_s(destination + 1, s(127));
            line(s(126) + " = lo;" + (wide ? " " + s(127) + " = hi;" : "") + " scc = (lo | hi) != 0u; }");
            return;
        }
        std::string value;
        const auto a = operand(ins, source);
        if (name == "s_not_b32") value = "~" + a;
        else if (name == "s_brev_b32") value = "reversebits(" + a + ")";
        else if (name == "s_bcnt1_i32_b32") value = "countbits(" + a + ")";
        else if (name == "s_bcnt1_i32_b64") value = "(countbits(" + a + ") + countbits(" + operand_high(ins, source) + "))";
        else if (name == "s_ff1_i32_b32") value = "firstbitlow(" + a + ")";
        else if (name == "s_flbit_i32_b32") value = "(" + a + " == 0u ? 0xffffffffu : 31u - firstbithigh(" + a + "))";
        else if (name == "s_sext_i32_i8") value = "(uint)(((int)(" + a + " << 24)) >> 24)";
        else if (name == "s_sext_i32_i16") value = "(uint)(((int)(" + a + " << 16)) >> 16)";
        else if (name == "s_abs_i32") value = "(uint)abs((int)" + a + ")";
        else if (name == "s_cmov_b32") {
            line("if (scc) " + s(destination) + " = " + a + ";");
            forget(destination);
            return;
        } else
            give_up(std::string(name));
        set_s(destination, value);
        if (name == "s_not_b32" || starts(name, "s_bcnt1") || name == "s_abs_i32")
            line("scc = " + s(destination) + " != 0u;");
    }
    void emit_sop2(const Ins &ins) {
        const auto destination = (ins.w0 >> 16) & 0x7f, first = ins.w0 & 0xff,
                   second = (ins.w0 >> 8) & 0xff;
        const auto name = ins.name;
        if (ends(name, "_b64") && name != "s_lshl_b64" && name != "s_lshr_b64" && name != "s_ashr_i64" && name != "s_bfm_b64" && name != "s_cselect_b64") {
            const auto combine = [&](const std::string &a, const std::string &b) -> std::string {
                if (name == "s_and_b64") return a + " & " + b;
                if (name == "s_or_b64") return a + " | " + b;
                if (name == "s_xor_b64") return a + " ^ " + b;
                if (name == "s_andn2_b64") return a + " & ~" + b;
                if (name == "s_orn2_b64") return a + " | ~" + b;
                if (name == "s_nand_b64") return "~(" + a + " & " + b + ")";
                if (name == "s_nor_b64") return "~(" + a + " | " + b + ")";
                if (name == "s_xnor_b64") return "~(" + a + " ^ " + b + ")";
                give_up(std::string(name));
            };
            line("{ uint lo = " + combine(operand(ins, first), operand(ins, second)) + ", hi = " +
                 combine(operand_high(ins, first), operand_high(ins, second)) + ";");
            set_s(destination, "lo");
            set_s(destination + 1, "hi");
            line("scc = (lo | hi) != 0u; }");
            return;
        }
        if (name == "s_lshl_b64" || name == "s_lshr_b64" || name == "s_ashr_i64" || name == "s_bfm_b64") {
            const auto count = operand(ins, second);
            const char *const how = name == "s_lshl_b64" ? "shift_left" : name == "s_lshr_b64" ? "shift_right" : "shift_signed";
            if (name == "s_bfm_b64")
                line("{ uint lo, hi; field_mask(" + operand(ins, first) + ", " + count + ", lo, hi);");
            else
                line("{ uint lo, hi; " + std::string(how) + "(" + operand(ins, first) + ", " + operand_high(ins, first) + ", " + count + ", lo, hi);");
            set_s(destination, "lo");
            set_s(destination + 1, "hi");
            line(name == "s_bfm_b64" ? "}" : "scc = (lo | hi) != 0u; }");
            return;
        }
        if (name == "s_cselect_b64") {
            line("{ uint lo = scc ? " + operand(ins, first) + " : " + operand(ins, second) + ", hi = scc ? " +
                 operand_high(ins, first) + " : " + operand_high(ins, second) + ";");
            set_s(destination, "lo");
            set_s(destination + 1, "hi");
            line("}");
            return;
        }
        const auto a = operand(ins, first), b = operand(ins, second);
        std::string value, condition = "X != 0u";
        if (name == "s_add_u32") value = a + " + " + b, condition = "X < " + a;
        else if (name == "s_sub_u32") value = a + " - " + b, condition = b + " > " + a;
        else if (name == "s_add_i32") value = a + " + " + b, condition = "false";
        else if (name == "s_sub_i32") value = a + " - " + b, condition = "false";
        else if (name == "s_addc_u32") value = a + " + " + b + " + (scc ? 1u : 0u)", condition = "X < " + a;
        else if (name == "s_and_b32") value = a + " & " + b;
        else if (name == "s_or_b32") value = a + " | " + b;
        else if (name == "s_xor_b32") value = a + " ^ " + b;
        else if (name == "s_andn2_b32") value = a + " & ~" + b;
        else if (name == "s_orn2_b32") value = a + " | ~" + b;
        else if (name == "s_nand_b32") value = "~(" + a + " & " + b + ")";
        else if (name == "s_nor_b32") value = "~(" + a + " | " + b + ")";
        else if (name == "s_xnor_b32") value = "~(" + a + " ^ " + b + ")";
        else if (name == "s_lshl_b32") value = a + " << (" + b + " & 31u)";
        else if (name == "s_lshr_b32") value = a + " >> (" + b + " & 31u)";
        else if (name == "s_ashr_i32") value = "(uint)((int)" + a + " >> (" + b + " & 31u))";
        else if (name == "s_mul_i32") value = a + " * " + b, condition.clear();
        else if (name == "s_min_i32") value = "(uint)min((int)" + a + ", (int)" + b + ")", condition = "(int)" + a + " <= (int)" + b;
        else if (name == "s_max_i32") value = "(uint)max((int)" + a + ", (int)" + b + ")", condition = "(int)" + a + " >= (int)" + b;
        else if (name == "s_min_u32") value = "min(" + a + ", " + b + ")", condition = a + " <= " + b;
        else if (name == "s_max_u32") value = "max(" + a + ", " + b + ")", condition = a + " >= " + b;
        else if (name == "s_cselect_b32") value = "(scc ? " + a + " : " + b + ")", condition.clear();
        else if (name == "s_bfe_i32") value = "(((" + b + " >> 16) & 127u) == 0u ? 0u : (uint)(((int)(" + a + " << (32u - ((" + b + " >> 16) & 127u) - (" + b + " & 31u)))) >> (32u - ((" + b + " >> 16) & 127u))))";
        else if (name == "s_bfe_u32") value = "((" + a + " >> (" + b + " & 31u)) & ((1u << ((" + b + " >> 16) & 127u)) - 1u))";
        else if (name == "s_bfm_b32") value = "((1u << (" + a + " & 31u)) - 1u) << (" + b + " & 31u)", condition.clear();
        else if (name == "s_mul_hi_i32") value = "mul_hi_signed(" + a + ", " + b + ")", condition.clear();
        else if (name == "s_lshl1_add_u32") value = "(" + a + " << 1) + " + b, condition = "false";
        else if (name == "s_lshl2_add_u32") value = "(" + a + " << 2) + " + b, condition = "false";
        else if (name == "s_lshl3_add_u32") value = "(" + a + " << 3) + " + b, condition = "false";
        else if (name == "s_lshl4_add_u32") value = "(" + a + " << 4) + " + b, condition = "false";
        else if (name == "s_pack_ll_b32_b16") value = "(" + a + " & 0xffffu) | (" + b + " << 16)", condition.clear();
        else if (name == "s_pack_hh_b32_b16") value = "(" + a + " >> 16) | (" + b + " & 0xffff0000u)", condition.clear();
        else if (name == "s_mul_hi_u32") value = "mul_hi(" + a + ", " + b + ")", condition.clear();
        else
            give_up(std::string(name));
        // an address in the code stays one with something added to it
        const bool code = (name == "s_add_u32" || name == "s_add_i32") &&
                          ((first < 128 && known_[first].kind == Known::code) ||
                           (second < 128 && known_[second].kind == Known::code));
        // the upper half of a root descriptor's address with a stride put in: on its way to
        // being a buffer descriptor
        Known upper;
        if (name == "s_or_b32" || name == "s_and_b32")
            for (const auto &[reg, other] : {std::pair{first, second}, std::pair{second, first}})
                if (reg < 128 && known_[reg].kind == Known::root && known_[reg].part == 1) {
                    upper = known_[reg];
                    if (const auto bits = constant_of(other, ins.literal); bits && name == "s_or_b32")
                        upper.stride = (*bits >> 16) & 0x3fff;
                }
        line("{ uint X = " + value + ";");
        if (!condition.empty())
            line("scc = " + condition + ";");
        set_s(destination, "X");
        line("}");
        if (code)
            known_[destination] = {Known::code};
        else if (upper.kind == Known::root)
            known_[destination] = upper;
    }
    void emit_sopk(const Ins &ins) {
        const auto destination = (ins.w0 >> 16) & 0x7f;
        const auto immediate = static_cast<std::uint32_t>(static_cast<std::int16_t>(ins.w0 & 0xffff));
        const auto name = ins.name;
        if (name == "s_movk_i32") {
            set_s(destination, hex(immediate));
            known_[destination] = {Known::constant, immediate};
        } else if (name == "s_addk_i32") {
            set_s(destination, s(destination) + " + " + hex(immediate));
        } else if (name == "s_mulk_i32") {
            set_s(destination, s(destination) + " * " + hex(immediate));
        } else if (starts(name, "s_cmpk_")) {
            const bool is_signed = name.find("_i32") != std::string_view::npos;
            const auto a = is_signed ? "(int)" + s(destination) : s(destination);
            const auto b = is_signed ? "(int)" + hex(immediate) : hex(ins.w0 & 0xffff);
            line("scc = " + a + " " + relation(name.substr(7, 2)) + " " + b + ";");
        } else if (starts(name, "s_setreg") || name == "s_getreg_b32" || name == "s_version") {
            // the GPU's own modes: nothing for the host
        } else {
            give_up(std::string(name));
        }
    }
    static std::string relation(std::string_view two) {
        if (two == "eq") return "==";
        if (two == "lg" || two == "ne") return "!=";
        if (two == "gt") return ">";
        if (two == "ge") return ">=";
        if (two == "lt") return "<";
        if (two == "le") return "<=";
        give_up("comparison " + std::string(two));
    }
    void emit_sopc(const Ins &ins) {
        const auto name = ins.name;
        const auto a = operand(ins, ins.w0 & 0xff), b = operand(ins, (ins.w0 >> 8) & 0xff);
        if (starts(name, "s_cmp_")) {
            const bool is_signed = ends(name, "_i32");
            line("scc = " + (is_signed ? "(int)" + a : a) + " " + relation(name.substr(6, 2)) + " " +
                 (is_signed ? "(int)" + b : b) + ";");
        } else if (name == "s_bitcmp0_b32") {
            line("scc = (" + a + " & (1u << (" + b + " & 31u))) == 0u;");
        } else if (name == "s_bitcmp1_b32") {
            line("scc = (" + a + " & (1u << (" + b + " & 31u))) != 0u;");
        } else {
            give_up(std::string(name));
        }
    }

    // ---- what a shader is given ----
    // The descriptors of a table are 32 bytes each, but for samplers, which are 16.
    std::uint32_t descriptor_size(std::uint32_t parameter) const {
        const auto &ranges = parameters_[parameter].ranges;
        return !ranges.empty() && ranges[0].type == 3 ? 16 : 32;
    }
    // what a register that an instruction takes a resource from stands for
    Resource resource_of(std::uint32_t reg) {
        if (reg >= 128)
            give_up("a resource that is not a descriptor of a table");
        const auto &known = known_[reg];
        if (known.kind == Known::root && known.part == 0) {
            // a buffer descriptor the shader made from the address of a root descriptor
            const auto &parameter = parameters_[known.value];
            Resource resource{parameter.type == 2 ? 2u : parameter.type == 3 ? 0u : 1u, parameter.reg, parameter.space};
            resource.root = true;
            if (reg + 1 < 128 && known_[reg + 1].kind == Known::root && known_[reg + 1].value == known.value)
                resource.stride = known_[reg + 1].stride;
            return resource;
        }
        if (known.kind != Known::descriptor || known.part != 0)
            give_up("a resource that is not a descriptor of a table");
        const auto &ranges = parameters_[known.value].ranges;
        if (known.dynamic < 0) {
            const auto index = known.offset / descriptor_size(known.value);
            for (const auto &range : ranges)
                if (index >= range.offset && (range.count == ~0u || index - range.offset < range.count))
                    return {range.type, range.base + (index - range.offset), range.space};
            give_up("a descriptor outside its table");
        }
        // One of many, picked while the shader runs. A table that has views of both kinds
        // at the same place leaves it to the shader which one a descriptor is: one it
        // writes through is an unordered access view.
        Resource resource;
        resource.dynamic = known.dynamic;
        resource.parameter = known.value;
        resource.origin = known.origin;
        bool readable = false, writable = false;
        for (const auto &range : ranges) {
            readable |= range.type == 0;
            writable |= range.type == 1;
        }
        if (ranges.empty())
            give_up("a descriptor outside its table");
        resource.type = readable && writable ? (facts_.written.count(known.origin) ? 1u : 0u) : ranges[0].type;
        return resource;
    }
    // a resource the shader writes through, which for one picked at run time settles what
    // it is declared as
    void written(const Resource &resource) {
        if (resource.type == 1)
            return;
        if (resource.dynamic >= 0 && !facts_.written.count(resource.origin)) {
            for (const auto &range : parameters_[resource.parameter].ranges)
                if (range.type == 1) {
                    facts_.written.insert(resource.origin);
                    throw Retry{};
                }
        }
        give_up("a store to something that is not an unordered access view");
    }
    // The name of a resource as the host's shader has it, declared the first time as `type`.
    // One picked at run time is an element of an array over a range of its table: each type
    // gets a range of its own, which is what the table has several for.
    std::string object(const Resource &resource, const std::string &type) {
        const char letter = "tubs"[resource.type];
        if (resource.dynamic < 0) {
            const auto name = std::string(1, letter) + std::to_string(resource.reg) + "_" + std::to_string(resource.space);
            const auto found = declared_.find(name);
            if (found != declared_.end()) {
                if (found->second != type)
                    give_up("one register as " + found->second + " and as " + type);
                return name;
            }
            declared_[name] = type;
            declarations_ += type + " " + name + " : register(" + letter + std::to_string(resource.reg) +
                             ", space" + std::to_string(resource.space) + ");\n";
            return name;
        }
        const auto &ranges = parameters_[resource.parameter].ranges;
        const auto key = std::to_string(resource.parameter) + letter + type;
        auto found = arrays_.find(key);
        if (found == arrays_.end()) {
            std::size_t chosen = ranges.size();
            for (std::size_t r = 0; r < ranges.size() && chosen == ranges.size(); ++r)
                if (ranges[r].type == resource.type && !taken_.count({resource.parameter, r}))
                    chosen = r;
            if (chosen == ranges.size())
                give_up("more kinds of resources picked at run time than their table has ranges for");
            taken_.insert({resource.parameter, chosen});
            const auto &range = ranges[chosen];
            const auto name = std::string(1, letter) + std::to_string(range.base) + "_" + std::to_string(range.space);
            if (declared_.count(name))
                give_up("one register as " + declared_[name] + " and as an array of " + type);
            declared_[name] = type + "[]";
            declarations_ += type + " " + name + "[" + (range.count == ~0u ? "" : std::to_string(range.count)) +
                             "] : register(" + letter + std::to_string(range.base) + ", space" +
                             std::to_string(range.space) + ");\n";
            found = arrays_.emplace(key, std::pair{name, range.offset}).first;
        }
        return found->second.first + "[" + slot(resource, found->second.second) + "]";
    }
    std::string slot(const Resource &resource, std::uint32_t first) {
        return "(d" + std::to_string(resource.dynamic) + " / " + std::to_string(descriptor_size(resource.parameter)) +
               "u - " + std::to_string(first) + "u)";
    }
    void emit_smem(const Ins &ins) {
        const auto data = (ins.w0 >> 6) & 0x7f, base = (ins.w0 & 0x3f) * 2;
        const auto offset = ins.w1 & 0x1fffff, index = ins.w1 >> 25;
        const bool indexed = index != 0x7d;
        const auto name = ins.name;
        const auto words_of = [&](std::string_view prefix) -> std::uint32_t {
            const auto rest = name.substr(prefix.size());
            return rest.empty() ? 1 : rest == "x2" ? 2 : rest == "x4" ? 4 : rest == "x8" ? 8 : 16;
        };
        std::string where = hex(offset);
        if (indexed)
            where = "(" + operand(ins, index) + " + " + where + ")";
        if (starts(name, "s_load_dword")) {
            const auto words = words_of("s_load_dword");
            const auto from = known_[base];
            if (from.kind == Known::code) {
                // constants kept with the code, after its last instruction
                uses_data_ = true;
                for (std::uint32_t n = 0; n < words; ++n)
                    set_s(data + n, "code_word(" + s(base) + " + " + where + " + " + std::to_string(4 * n) + "u)");
                return;
            }
            if (from.kind == Known::root && from.part == 0) {
                // read through the address of a root descriptor: the buffer's own words
                auto resource = resource_of(base);
                for (std::uint32_t n = 0; n < words; ++n)
                    set_s(data + n, read_word(resource, "0u", where, indexed ? ~0u : offset, 4 * n));
                return;
            }
            if (from.kind != Known::table)
                give_up("a load from memory that is not a descriptor of a table");
            Known loaded{Known::descriptor, from.value, offset};
            loaded.origin = ins.at;
            if (indexed) {
                // where in the table is only known when the shader runs
                loaded.dynamic = dynamics_++;
                line("d" + std::to_string(loaded.dynamic) + " = " + where + ";");
            }
            for (std::uint32_t n = 0; n < words; ++n)
                if (data + n < 128) {
                    known_[data + n] = loaded;
                    known_[data + n].part = n;
                }
            return;
        }
        if (starts(name, "s_buffer_load_dword")) {
            const auto words = words_of("s_buffer_load_dword");
            const auto resource = resource_of(base);
            for (std::uint32_t n = 0; n < words; ++n)
                set_s(data + n, read_word(resource, "0u", where, indexed ? ~0u : offset, 4 * n));
            return;
        }
        if (name == "s_gl1_inv" || name == "s_dcache_inv" || name == "s_dcache_wb")
            return;
        give_up(std::string(name));
    }
    // A word of a buffer: `offset` bytes into element `index`, plus `more`. `fixed` is the
    // offset when it is known now, and ~0 otherwise.
    std::string read_word(const Resource &resource, const std::string &index, const std::string &offset,
                          std::uint32_t fixed, std::uint32_t more) {
        const auto at = "(" + offset + " + " + std::to_string(more) + "u)";
        if (resource.type == 3)
            give_up("a buffer read through a sampler");
        if (resource.type == 2) {
            if (index != "0u")
                give_up("a constant buffer read by element");
            const auto name = constants(resource);
            // as many vectors as the shader is seen to read: the host's compiler is slow
            // with large ones
            auto &size = constants_size_[name];
            size = std::max(size, fixed == ~0u ? 4096u : (fixed + more) / 16 + 1);
            return name + "[" + at + " >> 4][(" + at + " >> 2) & 3u]";
        }
        if (resource.root) {
            const auto name = object(resource, resource.type == 1 ? "RWByteAddressBuffer" : "ByteAddressBuffer");
            return name + ".Load(" + bytes_of(resource, index) + at + ")";
        }
        return words(resource) + "[" + index + "].w[" + field(at, fixed, more) + "]";
    }
    // A buffer of the title's as the host's shader sees it: elements of words. The host's
    // view has the stride the title's descriptor gives, so an element is found without the
    // shader knowing its size, and an offset past an element's end reads on into the next,
    // as it does on the console.
    std::string words(const Resource &resource) {
        uses_words_ = true;
        return object(resource, resource.type == 1 ? "RWStructuredBuffer<Words>" : "StructuredBuffer<Words>");
    }
    static constexpr std::uint32_t words_in_element = 512;
    std::string field(const std::string &at, std::uint32_t fixed, std::uint32_t more) {
        if (fixed == ~0u)
            return at + " >> 2";
        if (fixed + more >= 4 * words_in_element)
            give_up("an offset beyond what an element of a buffer is declared with");
        return std::to_string((fixed + more) / 4);
    }
    void write_word(const Resource &resource, const std::string &index, const std::string &offset,
                    std::uint32_t fixed, std::uint32_t more, const std::string &value) {
        written(resource);
        const auto at = "(" + offset + " + " + std::to_string(more) + "u)";
        const std::string guard = exec_changes_ ? "if " + active() + " " : "";
        if (resource.root)
            line(guard + object(resource, "RWByteAddressBuffer") + ".Store(" + bytes_of(resource, index) + at + ", " + value + ");");
        else
            line(guard + words(resource) + "[" + index + "].w[" + field(at, fixed, more) + "] = " + value + ";");
    }
    // the bytes before element `index` of a root buffer, whose stride only the shader has
    std::string bytes_of(const Resource &resource, const std::string &index) {
        if (index == "0u")
            return "";
        if (!resource.stride)
            give_up("a buffer whose stride is not known");
        return index + " * " + std::to_string(resource.stride) + "u + ";
    }
    // a constant buffer as an array of vectors, by name
    std::string constants(const Resource &resource) {
        if (resource.dynamic >= 0) {
            uses_constants_ = true;
            return object(resource, "ConstantBuffer<Constants>") + ".w";
        }
        const auto name = "b" + std::to_string(resource.reg) + "_" + std::to_string(resource.space);
        if (!declared_.count(name)) {
            declared_[name] = "cbuffer";
            declarations_ += "cbuffer " + name + "_buffer : register(b" + std::to_string(resource.reg) +
                             ", space" + std::to_string(resource.space) + ") { uint4 " + name + "[@" + name + "@]; };\n";
        } else if (declared_[name] != "cbuffer") {
            give_up("one register as " + declared_[name] + " and as a constant buffer");
        }
        return name;
    }

    void fetch_inputs() {
        const auto &inputs = shader_.inputs;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const auto &element = inputs[i];
            const auto field = "input.f" + std::to_string(i);
            if (!_stricmp(element.name.c_str(), "SV_VertexID")) {
                line(v(0) + " = " + field + ";");
                vertex_id_ = true;
                continue;
            }
            if (!_stricmp(element.name.c_str(), "SV_InstanceID")) {
                line(v(3) + " = " + field + ";");
                instance_id_ = true;
                continue;
            }
            // element of register r: its components in v[4 + 4r] and on; what it lacks
            // reads 0, 0, 0, 1
            std::uint32_t component = 0;
            for (std::uint32_t channel = 0; channel < 4; ++channel) {
                const auto reg = 4 + 4 * element.reg + channel;
                if (reg > 255)
                    give_up("too many inputs");
                if (element.mask >> channel & 1) {
                    const auto name = field + (element.width > 1 ? std::string(".") + "xyzw"[component] : "");
                    line(v(reg) + " = " + (element.type == 3 ? u(name) : "(uint)" + name) + ";");
                    ++component;
                } else if (element.mask == (element.mask & -element.mask) * ((1u << element.width) - 1) &&
                           channel >= static_cast<std::uint32_t>(std::countr_zero(element.mask)) + element.width) {
                    line(v(reg) + " = " + (channel == 3 && element.type == 3 ? "0x3f800000u" : "0u") + ";");
                }
            }
        }
        if (!vertex_id_)
            line(v(0) + " = input.xv_vertex;");
        if (!instance_id_)
            line(v(3) + " = input.xv_instance;");
        fetched_ = true;
    }
    // component `channel` of the input in register `reg`, as bits
    std::string input_channel(std::uint32_t reg, std::uint32_t channel) {
        const auto &inputs = shader_.inputs;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const auto &element = inputs[i];
            if (element.reg != reg || !(element.mask >> channel & 1))
                continue;
            const auto component = std::popcount(static_cast<unsigned>(element.mask & ((1u << channel) - 1)));
            const auto name = "input.f" + std::to_string(i) + (element.width > 1 ? std::string(".") + "xyzw"[component] : "");
            return element.type == 3 ? u(name) : "(uint)" + name;
        }
        return "0u";
    }
    void emit_interpolation(const Ins &ins) {
        if (stage_ != Stage::pixel)
            give_up("interpolation outside a pixel shader");
        const auto destination = (ins.w0 >> 18) & 0xff, attribute = (ins.w0 >> 10) & 0x3f,
                   channel = (ins.w0 >> 8) & 3;
        const auto found = attributes_.find(attribute);
        // without the table, attribute n is the nth register that is not a system value's
        std::uint32_t reg = attribute;
        if (found != attributes_.end()) {
            reg = found->second;
        } else {
            std::vector<std::uint32_t> registers;
            for (const auto &element : shader_.inputs)
                if (!element.system_value && (registers.empty() || registers.back() != element.reg))
                    registers.push_back(element.reg);
            if (attribute >= registers.size())
                give_up("an attribute the signature does not have");
            reg = registers[attribute];
        }
        // the two steps of an interpolation both leave the interpolated value
        set_v(destination, input_channel(reg, channel));
    }

    // ---- vector instructions ----
    // The instructions with one or two sources in one word. A second word may say which
    // byte or half of each source is meant, and which of the destination is written.
    void emit_short(const Ins &ins) {
        if (ins.form == Ins::Form::lanes)
            give_up("an instruction that reads another lane");
        const auto destination = (ins.w0 >> 17) & 0xff, modifier = ins.modifier;
        const auto name = ins.name;
        if (name == "v_nop")
            return;
        std::string a, b;
        std::uint32_t mask = 106;
        if (ins.form == Ins::Form::sub_dword) {
            const bool half = ends(name, "_f16");
            const auto part = [&](std::uint32_t code, std::uint32_t bits) {
                const auto select = bits & 7;
                auto x = bits >> 7 & 1 ? operand(ins, code) : v(code);
                if (select < 4)
                    x = "((" + x + " >> " + std::to_string(8 * select) + ") & 0xffu)";
                else if (select < 6)
                    x = "((" + x + " >> " + std::to_string(16 * (select - 4)) + ") & 0xffffu)";
                if (bits >> 3 & 1 && select < 6) {
                    const auto shift = std::to_string(select < 4 ? 24 : 16);
                    x = "(uint)(((int)(" + x + " << " + shift + ")) >> " + shift + ")";
                }
                const auto sign = half ? "0x8000u" : "0x80000000u";
                if (bits >> 5 & 1)
                    x = "(" + x + " & ~" + sign + ")";
                if (bits >> 4 & 1)
                    x = "(" + x + " ^ " + sign + ")";
                return x;
            };
            a = part(modifier & 0xff, modifier >> 16);
            if (ins.format != F::vop1)
                b = part((ins.w0 >> 9) & 0xff, modifier >> 24);
            if (ins.format == F::vopc) {
                if (modifier >> 15 & 1)
                    mask = (modifier >> 8) & 0x7f;
            } else {
                if (modifier >> 13 & 7)
                    give_up(std::string(name) + " on part of a register, with a modifier");
                placed_ = {(modifier >> 8) & 7, (modifier >> 11) & 3};
            }
        } else {
            a = operand(ins, ins.w0 & 0x1ff);
            b = v((ins.w0 >> 9) & 0xff);
        }
        if (ins.format == F::vop1) {
            if (name != "v_nop")
                emit_vop1(name, destination, a);
        } else if (ins.format == F::vop2) {
            emit_vop2(ins, a, b);
        } else {
            emit_compare(name, mask, a, b);
        }
        placed_.reset();
    }
    // a value as the part of a register that an instruction writes
    std::string place(std::uint32_t reg, const std::string &value) {
        const auto [select, unused] = *placed_;
        if (select >= 6)
            return value;
        const std::uint32_t width = select < 4 ? 8 : 16, shift = select < 4 ? 8 * select : 16 * (select - 4);
        const auto part = "((" + value + ") & " + (width == 8 ? "0xffu" : "0xffffu") + ")";
        if (unused == 2) // the rest stays
            return "((" + v(reg) + " & " + hex(~((width == 8 ? 0xffu : 0xffffu) << shift)) + ") | (" + part + " << " + std::to_string(shift) + "))";
        if (unused == 1) // the sign fills what is above
            return "(uint)(((int)(" + part + " << " + std::to_string(32 - width) + ")) >> " + std::to_string(32 - width - shift) + ")";
        return "(" + part + " << " + std::to_string(shift) + ")";
    }
    // Memory the lanes share. Of it, only reading a register of another pixel of the same
    // quad is done: the pixel's own value and how it changes across the quad give it.
    void emit_shared(const Ins &ins) {
        const auto name = ins.name;
        const auto offset = ins.w0 & 0xffff;
        const auto source = (ins.w1 >> 8) & 0xff, destination = ins.w1 >> 24;
        if (name != "ds_swizzle_b32")
            give_up(std::string(name));
        if (stage_ != Stage::pixel || !(offset >> 15 & 1))
            give_up("ds_swizzle_b32 that is not between the pixels of a quad");
        const auto id = "x" + std::to_string(temporaries_++);
        if (const auto lane = offset & 3; (offset & 0xff) == lane * 0x55) {
            // Every pixel takes the same one's value, which is how a shader takes
            // differences across its quad. This pixel's own value stands for the first
            // pixel's and the others follow from it, so that the differences are right.
            line("float " + id + " = " + f(v(source)) + ";");
            set_v(destination, u(id + (lane & 1 ? " + ddx_coarse(" + id + ")" : "") +
                                 (lane & 2 ? " + ddy_coarse(" + id + ")" : "")));
            return;
        }
        if (!quad_) {
            std::size_t position = shader_.inputs.size();
            for (std::size_t i = 0; i < shader_.inputs.size(); ++i)
                if (!_stricmp(shader_.inputs[i].name.c_str(), "SV_Position"))
                    position = i;
            if (position == shader_.inputs.size())
                give_up("ds_swizzle_b32 in a shader without its position");
            const auto field = "input.f" + std::to_string(position);
            prologue_ += "    uint quad = ((uint)" + field + ".x & 1u) | (((uint)" + field + ".y & 1u) << 1);\n";
            quad_ = true;
        }
        line("uint " + id + "q = (" + hex(offset & 0xff) + " >> (quad * 2u)) & 3u; float " + id + " = " + f(v(source)) + ";");
        set_v(destination, u(id + " + ((float)(" + id + "q & 1u) - (float)(quad & 1u)) * ddx_fine(" + id +
                             ") + ((float)(" + id + "q >> 1) - (float)(quad >> 1)) * ddy_fine(" + id + ")"));
    }
    void emit_vop1(std::string_view name, std::uint32_t destination, const std::string &a) {
        std::string value;
        const auto fa = f(a);
        if (name == "v_nop") return;
        if (name == "v_swap_b32") {
            line(std::string(exec_changes_ ? "if " + active() + " " : "") + "{ uint X = " + v(destination) + "; " +
                 v(destination) + " = " + a + "; " + a + " = X; }");
            return;
        }
        if (name == "v_mov_b32") value = a;
        else if (name == "v_readfirstlane_b32") {
            set_s(destination & 0x7f, a);
            return;
        } else if (name == "v_cvt_i32_f32") value = "(uint)(int)clamp(" + fa + ", -2147483648.0, 2147483520.0)";
        else if (name == "v_cvt_u32_f32") value = "(uint)clamp(" + fa + ", 0.0, 4294967040.0)";
        else if (name == "v_cvt_f32_i32") value = u("(float)(int)" + a);
        else if (name == "v_cvt_f32_u32") value = u("(float)" + a);
        else if (name == "v_cvt_f16_f32") value = "f32tof16(" + fa + ")";
        else if (name == "v_cvt_f32_f16") value = u("f16tof32(" + a + ")");
        else if (name == "v_cvt_rpi_i32_f32") value = "(uint)(int)floor(" + fa + " + 0.5)";
        else if (name == "v_cvt_flr_i32_f32") value = "(uint)(int)floor(" + fa + ")";
        else if (name == "v_cvt_off_f32_i4") value = u("(float)(((int)(" + a + " << 28)) >> 28) / 16.0");
        else if (name == "v_cvt_f32_ubyte0") value = u("(float)(" + a + " & 255u)");
        else if (name == "v_cvt_f32_ubyte1") value = u("(float)((" + a + " >> 8) & 255u)");
        else if (name == "v_cvt_f32_ubyte2") value = u("(float)((" + a + " >> 16) & 255u)");
        else if (name == "v_cvt_f32_ubyte3") value = u("(float)(" + a + " >> 24)");
        else if (name == "v_fract_f32") value = u("frac(" + fa + ")");
        else if (name == "v_trunc_f32") value = u("trunc(" + fa + ")");
        else if (name == "v_ceil_f32") value = u("ceil(" + fa + ")");
        else if (name == "v_rndne_f32") value = u("round(" + fa + ")");
        else if (name == "v_floor_f32") value = u("floor(" + fa + ")");
        else if (name == "v_exp_f32") value = u("exp2(" + fa + ")");
        else if (name == "v_log_f32") value = u("log2(" + fa + ")");
        else if (name == "v_rcp_f32" || name == "v_rcp_iflag_f32") value = u("(1.0 / " + fa + ")");
        else if (name == "v_rsq_f32") value = u("rsqrt(" + fa + ")");
        else if (name == "v_sqrt_f32") value = u("sqrt(" + fa + ")");
        else if (name == "v_sin_f32") value = u("sin(" + fa + " * 6.28318530718)");
        else if (name == "v_cos_f32") value = u("cos(" + fa + " * 6.28318530718)");
        else if (name == "v_not_b32") value = "~" + a;
        else if (name == "v_bfrev_b32") value = "reversebits(" + a + ")";
        else if (name == "v_ffbh_u32") value = "(" + a + " == 0u ? 0xffffffffu : 31u - firstbithigh(" + a + "))";
        else if (name == "v_ffbl_b32") value = "firstbitlow(" + a + ")";
        else if (name == "v_floor_f16") value = "f32tof16(floor(f16tof32(" + a + ")))";
        else if (name == "v_ceil_f16") value = "f32tof16(ceil(f16tof32(" + a + ")))";
        else if (name == "v_rcp_f16") value = "f32tof16(1.0 / f16tof32(" + a + "))";
        else if (name == "v_sqrt_f16") value = "f32tof16(sqrt(f16tof32(" + a + ")))";
        else if (name == "v_rsq_f16") value = "f32tof16(rsqrt(f16tof32(" + a + ")))";
        else
            give_up(std::string(name));
        set_v(destination, value);
    }
    // the operations with two sources, shared by the 32-bit and the 64-bit encodings; `a`
    // is the first source, and `d` the destination's own value
    std::string binary(std::string_view name, const std::string &a, const std::string &b, const std::string &d) {
        const auto fa = f(a), fb = f(b), fd = f(d);
        const auto h = [](const std::string &x) { return "f16tof32(" + x + ")"; };
        const auto half = [](const std::string &x) { return "f32tof16(" + x + ")"; };
        if (name == "v_add_f32") return u(fa + " + " + fb);
        if (name == "v_sub_f32") return u(fa + " - " + fb);
        if (name == "v_subrev_f32") return u(fb + " - " + fa);
        if (name == "v_mul_f32") return u(fa + " * " + fb);
        if (name == "v_mul_legacy_f32") return u("mul_legacy(" + fa + ", " + fb + ")");
        if (name == "v_min_f32") return u("min(" + fa + ", " + fb + ")");
        if (name == "v_max_f32") return u("max(" + fa + ", " + fb + ")");
        if (name == "v_mac_f32" || name == "v_fmac_f32") return u(fa + " * " + fb + " + " + fd);
        if (name == "v_mac_legacy_f32") return u("mul_legacy(" + fa + ", " + fb + ") + " + fd);
        if (name == "v_add_f16") return half(h(a) + " + " + h(b));
        if (name == "v_sub_f16") return half(h(a) + " - " + h(b));
        if (name == "v_subrev_f16") return half(h(b) + " - " + h(a));
        if (name == "v_mul_f16") return half(h(a) + " * " + h(b));
        if (name == "v_min_f16") return half("min(" + h(a) + ", " + h(b) + ")");
        if (name == "v_max_f16") return half("max(" + h(a) + ", " + h(b) + ")");
        if (name == "v_fmac_f16") return half(h(a) + " * " + h(b) + " + " + h(d));
        if (name == "v_min_i32") return "(uint)min((int)" + a + ", (int)" + b + ")";
        if (name == "v_max_i32") return "(uint)max((int)" + a + ", (int)" + b + ")";
        if (name == "v_min_u32") return "min(" + a + ", " + b + ")";
        if (name == "v_max_u32") return "max(" + a + ", " + b + ")";
        if (name == "v_lshrrev_b32") return b + " >> (" + a + " & 31u)";
        if (name == "v_ashrrev_i32") return "(uint)((int)" + b + " >> (" + a + " & 31u))";
        if (name == "v_lshlrev_b32") return b + " << (" + a + " & 31u)";
        if (name == "v_and_b32") return a + " & " + b;
        if (name == "v_or_b32") return a + " | " + b;
        if (name == "v_xor_b32") return a + " ^ " + b;
        if (name == "v_xnor_b32") return "~(" + a + " ^ " + b + ")";
        if (name == "v_add_nc_u32") return a + " + " + b;
        if (name == "v_sub_nc_u32") return a + " - " + b;
        if (name == "v_subrev_nc_u32") return b + " - " + a;
        if (name == "v_mul_i32_i24") return "(uint)((((int)(" + a + " << 8)) >> 8) * (((int)(" + b + " << 8)) >> 8))";
        if (name == "v_mul_u32_u24") return "(" + a + " & 0xffffffu) * (" + b + " & 0xffffffu)";
        if (name == "v_mul_hi_u32_u24") return "mul_hi(" + a + " & 0xffffffu, " + b + " & 0xffffffu)";
        if (name == "v_mul_hi_i32_i24") return "mul_hi_signed((uint)(((int)(" + a + " << 8)) >> 8), (uint)(((int)(" + b + " << 8)) >> 8))";
        if (name == "v_cvt_pkrtz_f16_f32") return "(f32tof16(" + fa + ") | (f32tof16(" + fb + ") << 16))";
        if (name == "v_cvt_pknorm_i16_f32") return "(((uint)(int)round(clamp(" + fa + ", -1.0, 1.0) * 32767.0) & 0xffffu) | ((uint)(int)round(clamp(" + fb + ", -1.0, 1.0) * 32767.0) << 16))";
        if (name == "v_cvt_pknorm_u16_f32") return "((uint)round(saturate(" + fa + ") * 65535.0) | ((uint)round(saturate(" + fb + ") * 65535.0) << 16))";
        if (name == "v_ldexp_f32") return u("ldexp(" + fa + ", (float)(int)" + b + ")");
        give_up(std::string(name));
    }
    void emit_vop2(const Ins &ins, const std::string &a, const std::string &b) {
        const auto destination = (ins.w0 >> 17) & 0xff;
        const auto name = ins.name;
        if (name == "v_cndmask_b32") {
            set_v(destination, "(" + s(106) + " != 0u ? " + b + " : " + a + ")");
        } else if (name == "v_fmaak_f32" || name == "v_madak_f32") {
            set_v(destination, u(f(a) + " * " + f(b) + " + " + f(hex(ins.literal))));
        } else if (name == "v_fmamk_f32" || name == "v_madmk_f32") {
            set_v(destination, u(f(a) + " * " + f(hex(ins.literal)) + " + " + f(b)));
        } else if (name == "v_fmaak_f16") {
            set_v(destination, "f32tof16(f16tof32(" + a + ") * f16tof32(" + b + ") + f16tof32(" + hex(ins.literal) + "))");
        } else if (name == "v_add_co_ci_u32" || name == "v_sub_co_ci_u32" || name == "v_add_co_u32" || name == "v_sub_co_u32" || name == "v_subrev_co_u32") {
            carry(name, destination, 106, a, b, s(106));
        } else {
            set_v(destination, binary(name, a, b, v(destination)));
        }
    }
    // an addition or subtraction that leaves its carry in a mask
    void carry(std::string_view name, std::uint32_t destination, std::uint32_t mask, const std::string &a,
               const std::string &b, const std::string &in) {
        const bool subtract = starts(name, "v_sub"), reversed = starts(name, "v_subrev"), with_in = name.find("_ci_") != std::string_view::npos;
        const auto &x = reversed ? b : a, &y = reversed ? a : b;
        const auto extra = with_in ? "(" + in + " != 0u ? 1u : 0u)" : std::string("0u");
        if (subtract)
            line("{ uint cin = " + extra + ", X = " + x + " - " + y + " - cin; bool out = " + y + " > " + x +
                 " || (cin != 0u && " + y + " == " + x + ");");
        else
            line("{ uint cin = " + extra + ", X = " + x + " + " + y + " + cin; bool out = X < " + x +
                 " || (cin != 0u && X == " + x + ");");
        set_v(destination, "X");
        set_mask(mask, "out");
        line("}");
    }
    void emit_compare(std::string_view full, std::uint32_t mask, const std::string &a, const std::string &b) {
        // v_cmp[x]_<relation>_<type>
        const bool kills = starts(full, "v_cmpx_");
        auto rest = full.substr(kills ? 7 : 6);
        const auto split = rest.rfind('_');
        const auto how = rest.substr(0, split), type = rest.substr(split + 1);
        std::string condition;
        if (type == "f32" || type == "f16") {
            const auto x = type == "f16" ? "f16tof32(" + a + ")" : f(a), y = type == "f16" ? "f16tof32(" + b + ")" : f(b);
            if (how == "lt") condition = x + " < " + y;
            else if (how == "eq") condition = x + " == " + y;
            else if (how == "le") condition = x + " <= " + y;
            else if (how == "gt") condition = x + " > " + y;
            else if (how == "lg") condition = "(" + x + " < " + y + " || " + x + " > " + y + ")";
            else if (how == "ge") condition = x + " >= " + y;
            else if (how == "o") condition = "(" + x + " == " + x + " && " + y + " == " + y + ")";
            else if (how == "u") condition = "(" + x + " != " + x + " || " + y + " != " + y + ")";
            else if (how == "nge") condition = "!(" + x + " >= " + y + ")";
            else if (how == "nlg") condition = "!(" + x + " < " + y + " || " + x + " > " + y + ")";
            else if (how == "ngt") condition = "!(" + x + " > " + y + ")";
            else if (how == "nle") condition = "!(" + x + " <= " + y + ")";
            else if (how == "neq") condition = x + " != " + y;
            else if (how == "nlt") condition = "!(" + x + " < " + y + ")";
            else if (how == "f") condition = "false";
            else if (how == "tru") condition = "true";
            else if (how == "class") condition = "float_class(" + x + ", " + b + ")";
            else give_up(std::string(full));
        } else if (type == "i32" || type == "u32") {
            const auto x = type == "i32" ? "(int)" + a : a, y = type == "i32" ? "(int)" + b : b;
            if (how == "f") condition = "false";
            else if (how == "t") condition = "true";
            else condition = x + " " + relation(how) + " " + y;
        } else {
            give_up(std::string(full));
        }
        set_mask(mask, condition);
        if (kills) {
            line(s(126) + " &= " + s(mask) + ";");
            if (wave64_)
                line(s(127) + " = " + s(126) + ";");
        }
    }
    void emit_vop3(const Ins &ins) {
        const auto destination = ins.w0 & 0xff;
        const auto absolute = (ins.w0 >> 8) & 7, clamp = (ins.w0 >> 15) & 1;
        const auto negate = ins.w1 >> 29, modifier = (ins.w1 >> 27) & 3;
        const std::uint32_t codes[3] = {ins.w1 & 0x1ff, (ins.w1 >> 9) & 0x1ff, (ins.w1 >> 18) & 0x1ff};
        const auto name = ins.name;
        // which sources each operation has, so that no register is read that is not one
        const bool compare = ins.op < 0x100;
        const bool one = ins.op >= 0x180 && ins.op < 0x200;
        const bool two = compare || (ins.op >= 0x100 && ins.op < 0x140 && name != "v_cndmask_b32") || name == "v_mul_lo_u32" || name == "v_mul_hi_u32" || name == "v_mul_lo_i32" || name == "v_mul_hi_i32" || name == "v_ldexp_f32" || name == "v_add_co_u32" || name == "v_sub_co_u32" || name == "v_subrev_co_u32" || name == "v_cvt_pkrtz_f16_f32" || name == "v_cvt_pknorm_i16_f32" || name == "v_cvt_pknorm_u16_f32" || name == "v_add_nc_u16" || name == "v_sub_nc_u16" || name == "v_mul_lo_u16" || name == "v_lshlrev_b64" || name == "v_lshrrev_b64" || name == "v_readlane_b32" || name == "v_pack_b32_f16";
        const int count = one ? 1 : two ? 2 : 3;
        const bool is_float = name.find("_f32") != std::string_view::npos || name.find("_f16") != std::string_view::npos;
        const bool is_half = name.find("_f16") != std::string_view::npos && name.find("f16_f32") == std::string_view::npos;
        std::string x[3];
        for (int n = 0; n < count; ++n) {
            if (name == "v_cndmask_b32" && n == 2) { // the mask: a scalar register
                x[n] = operand(ins, codes[n]);
                continue;
            }
            x[n] = operand(ins, codes[n]);
            if ((absolute >> n & 1) || (negate >> n & 1)) {
                if ((!is_float && name != "v_cndmask_b32") || is_half)
                    give_up(std::string(name) + " with a modifier");
                auto value = f(x[n]);
                if (absolute >> n & 1)
                    value = "abs(" + value + ")";
                if (negate >> n & 1)
                    value = "-" + value;
                x[n] = u(value);
            }
        }
        std::string value;
        const auto fx = [&](int n) { return f(x[n]); };
        if (compare) {
            emit_compare(name, destination & 0x7f, x[0], x[1]);
            return;
        }
        if (one) {
            // through the same code as the short encoding, with the modifiers applied after
            const auto before = body_.size();
            emit_vop1(name, destination, x[0]);
            if (modifier || clamp)
                finish_float(before, destination, modifier, clamp, is_half);
            return;
        }
        if (name == "v_cndmask_b32") value = "(" + x[2] + " != 0u ? " + x[1] + " : " + x[0] + ")";
        else if (name == "v_add_co_u32" || name == "v_sub_co_u32" || name == "v_subrev_co_u32") {
            carry(name, destination, (ins.w0 >> 8) & 0x7f, x[0], x[1], "0u");
            return;
        } else if (two && ins.op >= 0x100 && ins.op < 0x140) value = binary(name, x[0], x[1], v(destination));
        else if (name == "v_mad_f32" || name == "v_fma_f32") value = u(fx(0) + " * " + fx(1) + " + " + fx(2));
        else if (name == "v_mad_legacy_f32") value = u("mul_legacy(" + fx(0) + ", " + fx(1) + ") + " + fx(2));
        else if (name == "v_fma_f16") value = "f32tof16(f16tof32(" + x[0] + ") * f16tof32(" + x[1] + ") + f16tof32(" + x[2] + "))";
        else if (name == "v_min3_f32") value = u("min(" + fx(0) + ", min(" + fx(1) + ", " + fx(2) + "))");
        else if (name == "v_max3_f32") value = u("max(" + fx(0) + ", max(" + fx(1) + ", " + fx(2) + "))");
        else if (name == "v_med3_f32") value = u("clamp(" + fx(0) + ", min(" + fx(1) + ", " + fx(2) + "), max(" + fx(1) + ", " + fx(2) + "))");
        else if (name == "v_min3_f16") value = "f32tof16(min(f16tof32(" + x[0] + "), min(f16tof32(" + x[1] + "), f16tof32(" + x[2] + "))))";
        else if (name == "v_max3_f16") value = "f32tof16(max(f16tof32(" + x[0] + "), max(f16tof32(" + x[1] + "), f16tof32(" + x[2] + "))))";
        else if (name == "v_med3_f16") value = "f32tof16(clamp(f16tof32(" + x[0] + "), min(f16tof32(" + x[1] + "), f16tof32(" + x[2] + ")), max(f16tof32(" + x[1] + "), f16tof32(" + x[2] + "))))";
        else if (name == "v_min3_i32") value = "(uint)min((int)" + x[0] + ", min((int)" + x[1] + ", (int)" + x[2] + "))";
        else if (name == "v_max3_i32") value = "(uint)max((int)" + x[0] + ", max((int)" + x[1] + ", (int)" + x[2] + "))";
        else if (name == "v_med3_i32") value = "(uint)clamp((int)" + x[0] + ", min((int)" + x[1] + ", (int)" + x[2] + "), max((int)" + x[1] + ", (int)" + x[2] + "))";
        else if (name == "v_min3_u32") value = "min(" + x[0] + ", min(" + x[1] + ", " + x[2] + "))";
        else if (name == "v_max3_u32") value = "max(" + x[0] + ", max(" + x[1] + ", " + x[2] + "))";
        else if (name == "v_med3_u32") value = "clamp(" + x[0] + ", min(" + x[1] + ", " + x[2] + "), max(" + x[1] + ", " + x[2] + "))";
        else if (name == "v_bfe_u32") value = "((" + x[0] + " >> (" + x[1] + " & 31u)) & ((" + x[2] + " & 31u) == 0u ? 0u : (0xffffffffu >> (32u - (" + x[2] + " & 31u)))))";
        else if (name == "v_bfe_i32") value = "((" + x[2] + " & 31u) == 0u ? 0u : (uint)(((int)(" + x[0] + " << (32u - (" + x[2] + " & 31u) - (" + x[1] + " & 31u)))) >> (32u - (" + x[2] + " & 31u))))";
        else if (name == "v_bfi_b32") value = "((" + x[0] + " & " + x[1] + ") | (~" + x[0] + " & " + x[2] + "))";
        else if (name == "v_mad_i32_i24") value = "(uint)((((int)(" + x[0] + " << 8)) >> 8) * (((int)(" + x[1] + " << 8)) >> 8)) + " + x[2];
        else if (name == "v_mad_u32_u24") value = "(" + x[0] + " & 0xffffffu) * (" + x[1] + " & 0xffffffu) + " + x[2];
        else if (name == "v_mul_lo_u32" || name == "v_mul_lo_i32") value = x[0] + " * " + x[1];
        else if (name == "v_mul_hi_u32") value = "mul_hi(" + x[0] + ", " + x[1] + ")";
        else if (name == "v_mul_hi_i32") value = "mul_hi_signed(" + x[0] + ", " + x[1] + ")";
        else if (name == "v_lshl_add_u32") value = "(" + x[0] + " << (" + x[1] + " & 31u)) + " + x[2];
        else if (name == "v_add_lshl_u32") value = "(" + x[0] + " + " + x[1] + ") << (" + x[2] + " & 31u)";
        else if (name == "v_add3_u32") value = x[0] + " + " + x[1] + " + " + x[2];
        else if (name == "v_lshl_or_b32") value = "(" + x[0] + " << (" + x[1] + " & 31u)) | " + x[2];
        else if (name == "v_and_or_b32") value = "(" + x[0] + " & " + x[1] + ") | " + x[2];
        else if (name == "v_or3_b32") value = x[0] + " | " + x[1] + " | " + x[2];
        else if (name == "v_xor3_b32") value = x[0] + " ^ " + x[1] + " ^ " + x[2];
        else if (name == "v_xad_u32") value = "(" + x[0] + " ^ " + x[1] + ") + " + x[2];
        else if (name == "v_cubeid_f32") value = u("cube_id(" + fx(0) + ", " + fx(1) + ", " + fx(2) + ")");
        else if (name == "v_cubesc_f32") value = u("cube_sc(" + fx(0) + ", " + fx(1) + ", " + fx(2) + ")");
        else if (name == "v_cubetc_f32") value = u("cube_tc(" + fx(0) + ", " + fx(1) + ", " + fx(2) + ")");
        else if (name == "v_cubema_f32") value = u("cube_ma(" + fx(0) + ", " + fx(1) + ", " + fx(2) + ")");
        else if (name == "v_ldexp_f32") value = u("ldexp(" + fx(0) + ", (float)(int)" + x[1] + ")");
        else if (name == "v_readlane_b32") {
            set_s(destination & 0x7f, x[0]);
            return;
        } else
            give_up(std::string(name));
        const auto before = body_.size();
        set_v(destination, value);
        if ((modifier || clamp) && is_float)
            finish_float(before, destination, modifier, clamp, is_half);
    }
    // the output modifier and the clamp of an instruction just emitted
    void finish_float(std::size_t, std::uint32_t destination, std::uint32_t modifier, std::uint32_t clamp, bool is_half) {
        const auto reg = v(destination);
        std::string value = is_half ? "f16tof32(" + reg + ")" : f(reg);
        if (modifier == 1) value = "(" + value + " * 2.0)";
        if (modifier == 2) value = "(" + value + " * 4.0)";
        if (modifier == 3) value = "(" + value + " * 0.5)";
        if (clamp) value = "saturate(" + value + ")";
        set_v(destination, is_half ? "f32tof16(" + value + ")" : u(value));
    }
    void emit_vop3p(const Ins &ins) {
        // two 16-bit halves at once; each source says which of its halves each half takes
        const auto destination = ins.w0 & 0xff;
        const auto select_low = (ins.w0 >> 11) & 7, select_high = ((ins.w1 >> 27) & 3) | ((ins.w0 >> 14) & 1) << 2;
        const auto negate_low = ins.w1 >> 29, negate_high = (ins.w0 >> 8) & 7;
        const std::uint32_t codes[3] = {ins.w1 & 0x1ff, (ins.w1 >> 9) & 0x1ff, (ins.w1 >> 18) & 0x1ff};
        const auto name = ins.name;
        const bool three = name == "v_pk_fma_f16" || name == "v_pk_mad_i16" || name == "v_pk_mad_u16" || starts(name, "v_fma_mix");
        std::string x[3];
        for (int n = 0; n < (three ? 3 : 2); ++n)
            x[n] = operand(ins, codes[n]);
        const auto half = [&](int n, bool high) {
            const auto select = (high ? select_high : select_low) >> n & 1;
            auto value = "f16tof32(" + x[n] + (select ? " >> 16" : "") + ")";
            if ((high ? negate_high : negate_low) >> n & 1)
                value = "-" + value;
            return value;
        };
        if (starts(name, "v_fma_mix")) {
            // a fused multiply and add of floats, each a half (as a float) or a whole one
            const auto mixed = [&](int n) {
                const bool whole = !(select_high >> n & 1);
                auto value = whole ? f(x[n]) : "f16tof32(" + x[n] + (select_low >> n & 1 ? " >> 16" : "") + ")";
                if (negate_high >> n & 1) value = "abs(" + value + ")";
                if (negate_low >> n & 1) value = "-" + value;
                return value;
            };
            const auto result = mixed(0) + " * " + mixed(1) + " + " + mixed(2);
            if (name == "v_fma_mix_f32") set_v(destination, u(result));
            else if (name == "v_fma_mixlo_f16") set_v(destination, "(" + v(destination) + " & 0xffff0000u) | f32tof16(" + result + ")");
            else set_v(destination, "(" + v(destination) + " & 0xffffu) | (f32tof16(" + result + ") << 16)");
            return;
        }
        const auto each = [&](bool high) -> std::string {
            if (name == "v_pk_mul_f16") return half(0, high) + " * " + half(1, high);
            if (name == "v_pk_add_f16") return half(0, high) + " + " + half(1, high);
            if (name == "v_pk_min_f16") return "min(" + half(0, high) + ", " + half(1, high) + ")";
            if (name == "v_pk_max_f16") return "max(" + half(0, high) + ", " + half(1, high) + ")";
            if (name == "v_pk_fma_f16") return half(0, high) + " * " + half(1, high) + " + " + half(2, high);
            give_up(std::string(name));
        };
        set_v(destination, "(f32tof16(" + each(false) + ") | (f32tof16(" + each(true) + ") << 16))");
    }

    // ---- textures and buffers ----
    void emit_image(const Ins &ins) {
        const auto name = ins.name;
        const auto dimension = (ins.w0 >> 3) & 7, mask = (ins.w0 >> 8) & 0xf;
        const bool half_address = ins.w1 >> 30 & 1, half_data = ins.w1 >> 31 & 1;
        const auto data = (ins.w1 >> 8) & 0xff;
        const auto texture = resource_of(((ins.w1 >> 16) & 0x1f) * 4);
        const std::string guard = exec_changes_ ? "if " + active() + " " : "";
        // the address: registers in a row from the first, or each one named
        std::vector<std::string> address;
        const auto more = (ins.w0 >> 1) & 3;
        address.push_back(v(ins.w1 & 0xff));
        if (more)
            for (std::uint32_t n = 0; n < more * 4; ++n)
                address.push_back(v((ins.extra[n / 4] >> (8 * (n % 4))) & 0xff));
        else
            for (std::uint32_t n = 1; n < 12; ++n)
                address.push_back("v" + std::to_string(((ins.w1 & 0xff) + n) & 0xff));
        std::size_t next = 0;
        const auto take = [&] {
            if (!more && next)
                used_v_.insert(((ins.w1 & 0xff) + next) & 0xff);
            return address.at(next++);
        };
        if (half_address) { // 16 bits each, two to a register
            std::vector<std::string> halves;
            for (std::size_t n = 0; n < 6; ++n) {
                const auto reg = n < address.size() ? address[n] : std::string("0u");
                if (!more && n)
                    used_v_.insert(((ins.w1 & 0xff) + n) & 0xff);
                halves.push_back("(" + reg + " & 0xffffu)");
                halves.push_back("(" + reg + " >> 16)");
            }
            address = halves;
        }
        static constexpr int coordinates[] = {1, 2, 3, 3, 2, 3, 3, 4};
        static constexpr const char *types[] = {"Texture1D", "Texture2D", "Texture3D", "TextureCube", "Texture1DArray", "Texture2DArray", "Texture2DMS", "Texture2DMSArray"};
        const int count = coordinates[dimension];
        const bool load = name == "image_load" || name == "image_load_mip" || name == "image_store" || name == "image_store_mip" || starts(name, "image_atomic") || name == "image_get_resinfo";
        std::string result; // a float4, or for a load whatever the texture has
        if (name == "image_get_resinfo") {
            // the size of a level, the layers or the depth, and how many levels there are
            const bool readable = texture.type != 1, multisampled = dimension >= 6;
            const auto object_name = object(texture, std::string(readable ? "" : "RW") + types[dimension] + "<float4>");
            static constexpr const char *sizes[] = {"x", "x, y", "x, y, z", "x, y", "x, z", "x, y, z", "x, y", "x, y, z"};
            const auto id = std::to_string(temporaries_++);
            line("uint x" + id + "x = 1u, x" + id + "y = 1u, x" + id + "z = 1u, x" + id + "w = 1u;");
            std::string arguments;
            for (const char *size = sizes[dimension]; *size; ++size)
                arguments += *size == ',' || *size == ' ' ? std::string(1, *size) : "x" + id + *size;
            if (multisampled)
                arguments += ", x" + id + "w";
            else if (readable)
                arguments = "(uint)" + take() + ", " + arguments + ", x" + id + "w";
            line(object_name + ".GetDimensions(" + arguments + ");");
            if (dimension == 3)
                line("x" + id + "z = 6u;");
            std::uint32_t reg = data;
            for (int c = 0; c < 4; ++c)
                if (mask >> c & 1)
                    set_v(reg++, "x" + id + "xyzw"[c]);
            return;
        }
        if (load) {
            if (dimension == 3)
                give_up("a cube read by texel");
            std::string where;
            const auto integer = [&](const std::string &reg) { return half_address ? "(int)" + reg : "(int)" + reg; };
            std::vector<std::string> xs;
            for (int n = 0; n < count; ++n)
                xs.push_back(integer(take()));
            const bool multisampled = dimension >= 6;
            const int places = multisampled ? count - 1 : count;
            where = "int" + std::to_string(places) + "(";
            for (int n = 0; n < places; ++n)
                where += (n ? ", " : "") + xs[n];
            where += ")";
            if (name == "image_load" || name == "image_load_mip") {
                const auto level = name == "image_load_mip" ? integer(take()) : std::string("0");
                if (texture.type == 1)
                    result = object(texture, std::string("RW") + types[dimension] + "<float4>") + "[" + where + "]";
                else if (multisampled)
                    result = object(texture, std::string(types[dimension]) + "<float4>") + ".Load(" + where + ", " + xs[count - 1] + ")";
                else
                    result = object(texture, std::string(types[dimension]) + "<float4>") + ".Load(int" + std::to_string(places + 1) + "(" + where + ", " + level + "))";
            } else if (name == "image_store" || name == "image_store_mip") {
                written(texture);
                const auto target = object(texture, std::string("RW") + types[dimension] + "<float4>");
                std::string value = "float4(";
                std::uint32_t reg = data;
                for (int c = 0; c < 4; ++c)
                    value += std::string(c ? ", " : "") + (mask >> c & 1 ? (half_data ? "f16tof32(" + v(reg++) + ")" : f(v(reg++))) : std::string("0.0"));
                value += ")";
                line(guard + target + "[" + where + "] = " + value + ";");
                return;
            } else {
                // an atomic operation on a texel, which leaves what was there before
                written(texture);
                const auto target = object(texture, std::string("RW") + types[dimension] + "<uint>") + "[" + where + "]";
                const auto id = "x" + std::to_string(temporaries_++);
                line("uint " + id + " = 0u;");
                line(guard + atomic(name.substr(13), target, v(data), v(data + 1), id));
                set_v(data, id);
                return;
            }
        } else {
            const auto sampler_register = ((ins.w1 >> 21) & 0x1f) * 4;
            auto state = resource_of(sampler_register);
            if (state.dynamic >= 0)
                state.type = 3;
            if (state.type != 3)
                give_up("a sampler that is not one");
            if (texture.type != 0)
                give_up("a sampled texture that is not a shader resource");
            // image_sample[_c][_d|_l|_b|_lz|_cl][_o], image_gather4[_c][_l|_lz][_o]
            const auto parts = name.substr(starts(name, "image_gather4") ? 13 : 12);
            const auto has = [&](std::string_view part) {
                const auto at = parts.find(part);
                return at != std::string_view::npos && (at + part.size() == parts.size() || parts[at + part.size()] == '_');
            };
            const bool gather = starts(name, "image_gather4"), compare = has("_c"), offset = has("_o"),
                       gradients = has("_d"), level = has("_l"), bias = has("_b"), zero = has("_lz");
            if (has("_cl"))
                give_up(std::string(name));
            std::string offset_value, bias_value, reference, ddx_value, ddy_value, level_value;
            const auto real = [&](const std::string &reg) { return half_address ? "f16tof32(" + reg + ")" : f(reg); };
            if (offset) {
                const auto packed = take();
                const int places = dimension == 2 ? 3 : 2;
                offset_value = ", int" + std::to_string(places) + "(";
                for (int n = 0; n < places; ++n)
                    offset_value += std::string(n ? ", " : "") + "(((int)(" + packed + " << " + std::to_string(26 - 8 * n) + ")) >> 26)";
                offset_value += ")";
            }
            if (bias) bias_value = real(take());
            if (compare) reference = real(take());
            const int gradient_places = dimension == 2 || dimension == 3 ? 3 : dimension == 0 || dimension == 4 ? 1 : 2;
            if (gradients) {
                std::string x = "float" + std::to_string(gradient_places) + "(", y = x;
                for (int n = 0; n < gradient_places; ++n)
                    x += (n ? ", " : "") + real(take());
                for (int n = 0; n < gradient_places; ++n)
                    y += (n ? ", " : "") + real(take());
                ddx_value = x + ")", ddy_value = y + ")";
            }
            if (dimension == 3 && gradients)
                give_up("a cube sampled with gradients");
            std::string where = "float" + std::to_string(count) + "(";
            for (int n = 0; n < count; ++n)
                where += (n ? ", " : "") + real(take());
            where += ")";
            // the GPU takes a cube's face and the place on it; the host a direction
            if (dimension == 3)
                where = "cube_direction(" + where + ")";
            if (level) level_value = real(take());
            const auto texture_name = object(texture, std::string(types[dimension]) + "<float4>");
            const auto sampler = object(state, compare ? "SamplerComparisonState" : "SamplerState");
            if (name == "image_get_lod") {
                if (stage_ != Stage::pixel)
                    give_up("image_get_lod outside a pixel shader");
                std::uint32_t reg = data;
                if (mask & 1)
                    set_v(reg++, u(texture_name + ".CalculateLevelOfDetail(" + sampler + ", " + where + ")"));
                if (mask & 2)
                    set_v(reg++, u(texture_name + ".CalculateLevelOfDetailUnclamped(" + sampler + ", " + where + ")"));
                return;
            }
            const auto &object = texture_name;
            if (gather) {
                const int component = std::countr_zero(mask);
                static constexpr const char *methods[] = {"GatherRed", "GatherGreen", "GatherBlue", "GatherAlpha"};
                result = object + "." + (compare ? "GatherCmp" : methods[component & 3]) + "(" + sampler + ", " + where +
                         (compare ? ", " + reference : "") + offset_value + ")";
                // the four texels, in the registers one after the other
                const auto sample = "x" + std::to_string(temporaries_++);
                line("float4 " + sample + " = " + result + ";");
                for (int c = 0; c < 4; ++c)
                    set_v(data + c, u(sample + "." + "xyzw"[c]));
                return;
            }
            if (compare) {
                if (!zero && !level)
                    give_up(std::string(name));
                result = "(" + object + ".SampleCmpLevelZero(" + sampler + ", " + where + ", " + reference + offset_value + ")).xxxx";
            } else if (gradients) {
                result = object + ".SampleGrad(" + sampler + ", " + where + ", " + ddx_value + ", " + ddy_value + offset_value + ")";
            } else if (zero || level || stage_ != Stage::pixel || !open_loops_.empty()) {
                result = object + ".SampleLevel(" + sampler + ", " + where + ", " + (level ? level_value : std::string("0.0")) + offset_value + ")";
            } else if (bias) {
                result = object + ".SampleBias(" + sampler + ", " + where + ", " + bias_value + offset_value + ")";
            } else {
                result = object + ".Sample(" + sampler + ", " + where + offset_value + ")";
            }
        }
        const auto sample = "x" + std::to_string(temporaries_++);
        line("float4 " + sample + " = " + result + ";");
        std::uint32_t reg = data;
        for (int c = 0; c < 4; ++c)
            if (mask >> c & 1) {
                const auto channel = sample + "." + "xyzw"[c];
                set_v(reg++, half_data ? "f32tof16(" + channel + ")" : u(channel));
            }
    }
    // An atomic operation named by the end of its instruction's name, as a statement:
    // `target` takes `value`, and `before` what was there.
    std::string atomic(std::string_view how, const std::string &target, const std::string &value,
                       const std::string &compare, const std::string &before) {
        const auto call = [&](const char *function, const std::string &with) {
            return std::string(function) + "(" + target + ", " + with + ", " + before + ");";
        };
        if (how == "add") return call("InterlockedAdd", value);
        if (how == "sub") return call("InterlockedAdd", "(uint)(-(int)" + value + ")");
        if (how == "inc") return call("InterlockedAdd", "1u");
        if (how == "dec") return call("InterlockedAdd", "0xffffffffu");
        if (how == "umin") return call("InterlockedMin", value);
        if (how == "umax") return call("InterlockedMax", value);
        if (how == "and") return call("InterlockedAnd", value);
        if (how == "or") return call("InterlockedOr", value);
        if (how == "xor") return call("InterlockedXor", value);
        if (how == "swap") return call("InterlockedExchange", value);
        if (how == "cmpswap") return call("InterlockedCompareExchange", compare + ", " + value);
        give_up("an atomic " + std::string(how));
    }
    void emit_buffer(const Ins &ins) {
        const auto name = ins.name;
        if (name == "buffer_gl0_inv" || name == "buffer_gl1_inv")
            return;
        const auto offset = ins.w0 & 0xfff;
        const bool has_offset = ins.w0 >> 12 & 1, has_index = ins.w0 >> 13 & 1;
        const auto address = ins.w1 & 0xff, data = (ins.w1 >> 8) & 0xff;
        const auto base = ((ins.w1 >> 16) & 0x1f) * 4, scalar_offset = ins.w1 >> 24;
        const auto resource = resource_of(base);
        const std::string guard = exec_changes_ ? "if " + active() + " " : "";
        std::string index = "0u", extra = hex(offset);
        std::uint32_t fixed = offset;
        if (has_index)
            index = v(address);
        if (has_offset)
            extra = "(" + v(address + (has_index ? 1 : 0)) + " + " + extra + ")", fixed = ~0u;
        if (scalar_offset != 0x80 && scalar_offset != 125)
            extra = "(" + operand(ins, scalar_offset) + " + " + extra + ")", fixed = ~0u;
        if (starts(name, "buffer_load_format_") || starts(name, "buffer_store_format_")) {
            // a buffer of elements with a format: element `index`
            const bool store = starts(name, "buffer_store");
            const auto components = name.size() - (store ? 20 : 19);
            if (resource.root || resource.type >= 2)
                give_up("a buffer with a format that is not a view");
            if (ends(name.substr(0, name.size() - components - 1), "d16"))
                give_up(std::string(name));
            if (store) {
                written(resource);
                std::string value = "float4(";
                for (std::size_t c = 0; c < 4; ++c)
                    value += std::string(c ? ", " : "") + (c < components ? f(v(data + static_cast<std::uint32_t>(c))) : std::string("0.0"));
                line(guard + object(resource, "RWBuffer<float4>") + "[" + index + "] = " + value + ");");
                return;
            }
            const auto buffer = object(resource, resource.type == 1 ? "RWBuffer<float4>" : "Buffer<float4>");
            const auto sample = "x" + std::to_string(temporaries_++);
            line("float4 " + sample + " = " + buffer + (resource.type == 1 ? "[" + index + "]" : ".Load(" + index + ")") + ";");
            for (std::size_t c = 0; c < components; ++c)
                set_v(data + static_cast<std::uint32_t>(c), u(sample + "." + "xyzw"[c]));
            return;
        }
        const auto count = [&](std::string_view prefix) -> std::uint32_t {
            const auto rest = name.substr(prefix.size());
            return rest.empty() ? 1 : rest == "x2" ? 2 : rest == "x3" ? 3 : rest == "x4" ? 4 : 0;
        };
        if (starts(name, "buffer_load_dword") && count("buffer_load_dword")) {
            for (std::uint32_t n = 0, words = count("buffer_load_dword"); n < words; ++n)
                set_v(data + n, read_word(resource, index, extra, fixed, 4 * n));
            return;
        }
        if (starts(name, "buffer_store_dword") && count("buffer_store_dword")) {
            for (std::uint32_t n = 0, words = count("buffer_store_dword"); n < words; ++n)
                write_word(resource, index, extra, fixed, 4 * n, v(data + n));
            return;
        }
        if (name == "buffer_load_ubyte" || name == "buffer_load_sbyte" || name == "buffer_load_ushort" ||
            name == "buffer_load_sshort") {
            // part of a word, from where in it the offset says
            const bool byte = ends(name, "byte"), extend = name[12] == 's';
            const auto shift = "((" + extra + " & 3u) * 8u)";
            const auto part = "((" + read_word(resource, index, extra, fixed == ~0u ? fixed : fixed & ~3u, 0) + " >> " + shift + ") & " + (byte ? "0xffu" : "0xffffu") + ")";
            const auto bits = byte ? "24" : "16";
            set_v(data, extend ? "(uint)(((int)(" + part + " << " + bits + ")) >> " + bits + ")" : part);
            return;
        }
        if (starts(name, "buffer_atomic_") && !ends(name, "_x2")) {
            written(resource);
            const auto at = "(" + extra + " + 0u)";
            const auto id = "x" + std::to_string(temporaries_++);
            line("uint " + id + " = 0u;");
            if (resource.root) {
                const auto how = name.substr(14);
                static constexpr std::pair<const char *, const char *> methods[] = {
                    {"add", "InterlockedAdd"}, {"umin", "InterlockedMin"}, {"umax", "InterlockedMax"},
                    {"and", "InterlockedAnd"}, {"or", "InterlockedOr"}, {"xor", "InterlockedXor"},
                    {"swap", "InterlockedExchange"}};
                const char *method = nullptr;
                for (const auto &[suffix, function] : methods)
                    if (how == suffix)
                        method = function;
                if (!method)
                    give_up("an atomic " + std::string(how) + " on a root buffer");
                line(guard + object(resource, "RWByteAddressBuffer") + "." + method + "(" + bytes_of(resource, index) + at + ", " + v(data) + ", " + id + ");");
            } else {
                line(guard + atomic(name.substr(14), words(resource) + "[" + index + "].w[" + field(at, fixed, 0) + "]", v(data), v(data + 1), id));
            }
            set_v(data, id);
            return;
        }
        give_up(std::string(name));
    }

    void emit_export(const Ins &ins) {
        const auto target = (ins.w0 >> 4) & 0x3f, enabled = ins.w0 & 0xf;
        const bool compressed = ins.w0 >> 10 & 1, valid_mask = ins.w0 >> 12 & 1;
        std::string channels[4] = {"0.0", "0.0", "0.0", "0.0"};
        if (compressed) {
            for (int pair = 0; pair < 2; ++pair) {
                if (!(enabled >> (2 * pair) & 3))
                    continue;
                const auto reg = v((ins.w1 >> (8 * pair)) & 0xff);
                channels[2 * pair] = "f16tof32(" + reg + ")";
                channels[2 * pair + 1] = "f16tof32(" + reg + " >> 16)";
            }
        } else {
            for (int n = 0; n < 4; ++n)
                if (enabled >> n & 1)
                    channels[n] = f(v((ins.w1 >> (8 * n)) & 0xff));
        }
        const auto &outputs = shader_.outputs;
        const auto write = [&](std::size_t field, std::uint32_t from) {
            const auto &element = outputs[field];
            std::string value;
            std::uint32_t places = 0;
            for (std::uint32_t channel = from; channel < 4 && places < element.width; ++channel, ++places)
                value += (places ? ", " : "") + (element.type == 3 ? channels[channel] : "asuint(" + channels[channel] + ")");
            for (; places < element.width; ++places)
                value += ", 0";
            const std::string type = element.type == 3 ? "float" : element.type == 2 ? "int" : "uint";
            line(std::string(exec_changes_ ? "if " + active() + " " : "") + "output.f" + std::to_string(field) + " = " +
                 (element.width > 1 ? type + std::to_string(element.width) + "(" + value + ")" : "(" + type + ")(" + value + ")") + ";");
        };
        bool written = false;
        if (stage_ == Stage::pixel && valid_mask && exec_changes_)
            line("if (!" + active() + ") discard;");
        if (stage_ == Stage::vertex && target == 12) {
            for (std::size_t i = 0; i < outputs.size(); ++i)
                if (!_stricmp(outputs[i].name.c_str(), "SV_Position"))
                    write(i, 0), written = true;
        } else if (stage_ == Stage::vertex && target >= 32) {
            // parameter n: the nth register that is not the position's
            std::vector<std::uint32_t> registers;
            for (const auto &element : outputs)
                if (_stricmp(element.name.c_str(), "SV_Position") &&
                    std::find(registers.begin(), registers.end(), element.reg) == registers.end())
                    registers.push_back(element.reg);
            std::sort(registers.begin(), registers.end());
            if (target - 32 < registers.size())
                for (std::size_t i = 0; i < outputs.size(); ++i)
                    if (outputs[i].reg == registers[target - 32] && _stricmp(outputs[i].name.c_str(), "SV_Position"))
                        write(i, static_cast<std::uint32_t>(std::countr_zero(static_cast<unsigned>(outputs[i].mask ? outputs[i].mask : 1)))), written = true;
            if (!written)
                return; // a parameter nothing after this stage reads
        } else if (stage_ == Stage::vertex && (target == 13 || target == 20)) {
            return; // more of the position (clip distances, point size) and the primitive's
        } else if (stage_ == Stage::pixel && target < 8) {
            for (std::size_t i = 0; i < outputs.size(); ++i)
                if (!_stricmp(outputs[i].name.c_str(), "SV_Target") && outputs[i].index == target)
                    write(i, 0), written = true;
            if (!written)
                return;
        } else if (stage_ == Stage::pixel && target == 8) {
            for (std::size_t i = 0; i < outputs.size(); ++i)
                if (!_stricmp(outputs[i].name.c_str(), "SV_Depth"))
                    write(i, 0), written = true;
            if (!written)
                return;
        } else if (target == 9) {
            return; // the null target: only its valid mask counts
        }
        if (!written)
            give_up("export to target " + std::to_string(target));
        exported_ = true;
    }

    // ---- the whole shader ----
    static std::string field_type(const Element &element) {
        return std::string(element.type == 1 ? "uint" : element.type == 2 ? "int" : "float") +
               (element.width > 1 ? std::to_string(element.width) : "");
    }
    static bool generated(const Element &element) {
        for (const auto *made : {"SV_VertexID", "SV_InstanceID", "SV_PrimitiveID", "SV_IsFrontFace", "SV_SampleIndex", "SV_Coverage"})
            if (!_stricmp(element.name.c_str(), made))
                return true;
        return false;
    }
    std::string structure(const char *name, const std::vector<Element> &elements, bool interpolated, const std::string &more) {
        std::string text = std::string("struct ") + name + " {\n";
        for (const bool last : {false, true})
            for (std::size_t field = 0; field < elements.size(); ++field) {
                const auto &element = elements[field];
                if (generated(element) != last)
                    continue;
                const bool integer = element.type == 1 || element.type == 2;
                const bool named = !_strnicmp(element.name.c_str(), "SV_", 3);
                text += "    ";
                if (interpolated && integer && !named)
                    text += "nointerpolation ";
                text += field_type(element) + " f" + std::to_string(field) + " : " + element.name;
                if (!named || element.index || !_stricmp(element.name.c_str(), "SV_Target") ||
                    !_stricmp(element.name.c_str(), "SV_ClipDistance") || !_stricmp(element.name.c_str(), "SV_CullDistance"))
                    text += std::to_string(element.index);
                text += ";\n";
            }
        return text + more + "};\n";
    }
    std::string finish() {
        if (stage_ != Stage::compute && !exported_)
            give_up("nothing exported");
        if (stage_ == Stage::vertex && !fetched_ && !shader_.inputs.empty())
            give_up("a vertex shader that does not fetch its inputs");
        std::string text = helpers;
        if (uses_words_)
            text += "struct Words { uint w[" + std::to_string(words_in_element) + "]; };\n";
        if (uses_constants_)
            text += "struct Constants { uint4 w[4096]; };\n";
        if (uses_data_) {
            // the constants a shader keeps after its code, by their address from the first
            // instruction
            const auto count = std::min<std::size_t>(code_.size() - data_first_, 4096);
            if (!count)
                give_up("constants after the code that are not there");
            text += "static const uint code_data[" + std::to_string(count) + "] = {";
            for (std::size_t n = 0; n < count; ++n)
                text += (n ? ", " : "") + hex(code_[data_first_ + n]);
            text += "};\nuint code_word(uint address) { return code_data[min((address >> 2) - " +
                    std::to_string(data_first_) + "u, " + std::to_string(count - 1) + "u)]; }\n";
        }
        for (const auto &[name, size] : constants_size_) {
            const auto mark = "@" + name + "@";
            if (const auto at = declarations_.find(mark); at != std::string::npos)
                declarations_.replace(at, mark.size(), std::to_string(std::min(size, 4096u)));
        }
        text += declarations_;
        std::string registers;
        for (int n = 0; n < dynamics_; ++n)
            registers += "    uint d" + std::to_string(n) + " = 0u;\n";
        for (const auto reg : used_s_)
            registers += "    uint s" + std::to_string(reg) + " = " + (reg == 126 || reg == 127 ? "0xffffffffu" : "0u") + ";\n";
        for (const auto reg : used_v_)
            registers += "    uint v" + std::to_string(reg) + " = 0u;\n";
        registers += "    bool scc = false;\n    uint next = 0u;\n";
        if (stage_ == Stage::compute) {
            text += "[numthreads(" + std::to_string(std::max(header_[1], 1u)) + ", " + std::to_string(std::max(header_[2], 1u)) +
                    ", " + std::to_string(std::max(header_[3], 1u)) + ")]\nvoid main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {\n" +
                    registers + prologue_ + body_ + "}\n";
            return text;
        }
        std::string more;
        if (stage_ == Stage::vertex) {
            if (!vertex_id_) more += "    uint xv_vertex : SV_VertexID;\n";
            if (!instance_id_) more += "    uint xv_instance : SV_InstanceID;\n";
        }
        const bool no_input = shader_.inputs.empty() && more.empty();
        if (!no_input)
            text += structure("Input", shader_.inputs, stage_ == Stage::pixel, more);
        text += structure("Output", shader_.outputs, false, "");
        text += std::string("Output main(") + (no_input ? "" : "Input input") + ") {\n    Output output = (Output)0;\n" +
                registers + prologue_ + body_ + "    return output;\n}\n";
        return text;
    }

    static constexpr const char *helpers =
        "float mul_legacy(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }\n"
        "uint mul_hi(uint a, uint b) { uint al = a & 0xffffu, ah = a >> 16, bl = b & 0xffffu, bh = b >> 16;\n"
        "    uint ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;\n"
        "    uint mid = (ll >> 16) + (lh & 0xffffu) + (hl & 0xffffu);\n"
        "    return hh + (lh >> 16) + (hl >> 16) + (mid >> 16); }\n"
        "uint mul_hi_signed(uint a, uint b) { uint high = mul_hi(a, b);\n"
        "    if ((int)a < 0) high -= b;\n"
        "    if ((int)b < 0) high -= a;\n"
        "    return high; }\n"
        "float cube_ma(float x, float y, float z) { float3 a = abs(float3(x, y, z)); return 2.0 * (a.z >= a.x && a.z >= a.y ? z : a.y >= a.x ? y : x); }\n"
        "float cube_id(float x, float y, float z) { float3 a = abs(float3(x, y, z)); return a.z >= a.x && a.z >= a.y ? (z < 0.0 ? 5.0 : 4.0) : a.y >= a.x ? (y < 0.0 ? 3.0 : 2.0) : (x < 0.0 ? 1.0 : 0.0); }\n"
        "float cube_sc(float x, float y, float z) { float3 a = abs(float3(x, y, z)); return a.z >= a.x && a.z >= a.y ? (z < 0.0 ? -x : x) : a.y >= a.x ? x : (x < 0.0 ? z : -z); }\n"
        "float cube_tc(float x, float y, float z) { float3 a = abs(float3(x, y, z)); return a.z >= a.x && a.z >= a.y ? -y : a.y >= a.x ? (y < 0.0 ? -z : z) : -y; }\n"
        "void shift_left(uint lo, uint hi, uint n, out uint rl, out uint rh) { n &= 63u;\n"
        "    if (n == 0u) { rl = lo; rh = hi; } else if (n < 32u) { rl = lo << n; rh = (hi << n) | (lo >> (32u - n)); } else { rl = 0u; rh = lo << (n - 32u); } }\n"
        "void shift_right(uint lo, uint hi, uint n, out uint rl, out uint rh) { n &= 63u;\n"
        "    if (n == 0u) { rl = lo; rh = hi; } else if (n < 32u) { rl = (lo >> n) | (hi << (32u - n)); rh = hi >> n; } else { rl = hi >> (n - 32u); rh = 0u; } }\n"
        "void shift_signed(uint lo, uint hi, uint n, out uint rl, out uint rh) { n &= 63u;\n"
        "    if (n == 0u) { rl = lo; rh = hi; } else if (n < 32u) { rl = (lo >> n) | (hi << (32u - n)); rh = (uint)((int)hi >> n); } else { rl = (uint)((int)hi >> (n - 32u)); rh = (uint)((int)hi >> 31); } }\n"
        "void field_mask(uint count, uint first, out uint rl, out uint rh) { count &= 63u;\n"
        "    uint lo = count >= 32u ? 0xffffffffu : (1u << count) - 1u, hi = count > 32u ? (1u << (count - 32u)) - 1u : 0u;\n"
        "    shift_left(lo, hi, first, rl, rh); }\n"
        "float3 cube_direction(float3 place) { float2 at = (place.xy - 1.5) * 2.0; int face = (int)round(place.z);\n"
        "    return face == 0 ? float3(1.0, -at.y, -at.x) : face == 1 ? float3(-1.0, -at.y, at.x) : face == 2 ? float3(at.x, 1.0, at.y)\n"
        "         : face == 3 ? float3(at.x, -1.0, -at.y) : face == 4 ? float3(at.x, -at.y, 1.0) : float3(-at.x, -at.y, -1.0); }\n"
        "bool float_class(float x, uint mask) { uint b = asuint(x); bool neg = (b >> 31) != 0u; uint e = (b >> 23) & 255u; uint m = b & 0x7fffffu;\n"
        "    uint c = e == 255u ? (m != 0u ? ((m >> 22) != 0u ? 2u : 1u) : (neg ? 4u : 512u)) : e == 0u ? (m == 0u ? (neg ? 32u : 64u) : (neg ? 16u : 128u)) : (neg ? 8u : 256u);\n"
        "    return (mask & c) != 0u; }\n";

    Stage stage_;
    const Shader &shader_;
    Facts &facts_;
    int dynamics_ = 0;
    std::map<std::string, std::pair<std::string, std::uint32_t>> arrays_; // name and first slot
    std::set<std::pair<std::uint32_t, std::size_t>> taken_;             // ranges with an array
    std::optional<std::pair<std::uint32_t, std::uint32_t>> placed_;
    std::map<std::string, std::uint32_t> constants_size_; // in vectors, by name
    bool uses_words_ = false, uses_constants_ = false, uses_data_ = false, quad_ = false;
    std::vector<Parameter> parameters_;
    std::vector<std::uint32_t> code_;
    std::uint32_t header_[4] = {};
    std::vector<Ins> code_list_;
    std::map<std::uint32_t, std::size_t> index_of_; // by an instruction's place, in words
    std::uint32_t data_first_ = 0;                  // the word after the last instruction
    std::vector<Block> blocks_;
    std::vector<std::size_t> target_block_;
    std::map<std::size_t, std::size_t> loop_end_;
    std::vector<std::size_t> open_loops_;
    Known known_[128];
    std::map<std::uint32_t, std::uint32_t> attributes_;
    std::set<std::uint32_t> used_s_, used_v_;
    std::map<std::string, std::string> declared_;
    std::string declarations_, prologue_, body_, indent_ = "    ";
    int temporaries_ = 0;
    bool exec_changes_ = false, wave64_ = false;
    bool fetched_ = false, exported_ = false, vertex_id_ = false, instance_id_ = false;
};
} // namespace

std::optional<std::string> translate(Stage stage, const Shader &shader, std::string *why) {
    if (stage != Stage::vertex && stage != Stage::pixel && stage != Stage::compute) {
        if (why)
            *why = "stage";
        return std::nullopt;
    }
    try {
        // not on the stack: it is large, and a title's threads may have little; and again
        // with what a pass found out, until a pass finds nothing new
        Facts facts;
        for (int pass = 0; pass < 64; ++pass) {
            try {
                return std::make_unique<Translator>(stage, shader, facts)->run();
            } catch (const Retry &) {
            }
        }
        give_up("descriptors that do not settle");
    } catch (const Unsupported &unsupported) {
        if (why)
            *why = unsupported.what;
    } catch (const std::exception &error) {
        if (why)
            *why = error.what();
    }
    return std::nullopt;
}
} // namespace xv::runtime::modules::rdna
