#include "runtime/modules/console_shader_rdna.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <memory>
#include <set>

// From the console GPU's machine code (AMD's RDNA instruction set) to HLSL. Only straight
// code is taken: the instructions are run once over symbolic registers, each write becoming
// one HLSL statement on 32-bit patterns. Anything not known here (a branch, an instruction,
// a register read before it is written) gives up on the whole shader.
//
// How a shader reaches what it is given, as read from a Series title's shaders:
// - the root signature's parameters are in the scalar registers from s8 on, in order: two
//   registers for a root descriptor (its address), one for a descriptor table (a pointer),
//   one for each root constant;
// - a table is 32 bytes a texture and 16 bytes a buffer or a sampler;
// - a vertex shader calls the fetch shader (s_swappc through s4) first, which leaves input
//   element n in v[4 + 4n] and the three after it;
// - a pixel shader has the barycentrics in v0 and v1, and interpolates attribute n, which is
//   the nth input element that is not a system value.
namespace xs::runtime::modules::rdna {
namespace {
constexpr std::uint32_t first_user_register = 8;
struct Value {
    enum Kind {
        undefined,
        opaque,     // masks and the like: carried around, never computed with
        expression, // an HLSL expression of type uint
        table,      // pointer to the descriptor table of root parameter `a`
        address_lo, // low and high half of the address of root descriptor `a`
        address_hi,
        descriptor, // word `c` of a descriptor: of table `a` at index `text` (an
                    // expression), or of root descriptor `a` when `b` is set
        packed,     // two floats as halves: `text` and `other`
    } kind = undefined;
    std::uint32_t a = 0, b = 0, c = 0;
    std::string text, other;
};
Value expression(std::string text) { return {Value::expression, 0, 0, 0, std::move(text), {}}; }
Value constant(std::uint32_t bits) { return expression(std::to_string(bits) + "u"); }

struct Range {
    std::uint32_t type, count, base, space, offset; // type: 0 SRV, 1 UAV, 2 CBV, 3 sampler
};
struct Parameter {
    std::uint32_t type = 0; // 0 table, 1 constants, 2 CBV, 3 SRV, 4 UAV
    std::uint32_t reg = 0, space = 0, count = 0;
    std::vector<Range> ranges;
    std::uint32_t first_register = 0; // the scalar register it starts at
};
std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t at) {
    std::uint32_t value = 0;
    if (at + 4 <= bytes.size())
        std::memcpy(&value, bytes.data() + at, 4);
    return value;
}
bool read_root_signature(std::span<const std::uint8_t> data, std::vector<Parameter> &parameters) {
    const auto version = word(data, 0), count = word(data, 4), at = word(data, 8);
    if ((version != 1 && version != 2) || count > 64 || at + 12ull * count > data.size())
        return false;
    std::uint32_t next = first_user_register;
    for (std::uint32_t i = 0; i < count; ++i) {
        Parameter parameter;
        parameter.type = word(data, at + 12 * i);
        const auto offset = word(data, at + 12 * i + 8);
        parameter.first_register = next;
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
            next += 1;
        } else if (parameter.type == 1) {
            parameter.reg = word(data, offset);
            parameter.space = word(data, offset + 4);
            parameter.count = word(data, offset + 8);
            next += parameter.count;
        } else if (parameter.type <= 4) {
            parameter.reg = word(data, offset);
            parameter.space = word(data, offset + 4);
            next += 2;
        } else {
            return false;
        }
        parameters.push_back(std::move(parameter));
    }
    return true;
}

struct Unsupported {};
class Translator {
  public:
    Translator(Stage stage, const Shader &shader) : stage_(stage), shader_(shader) {}
    std::string run() {
        if (!read_root_signature(shader_.root_signature, parameters_))
            throw Unsupported{};
        for (std::uint32_t p = 0; p < parameters_.size(); ++p) {
            const auto &parameter = parameters_[p];
            const auto at = parameter.first_register;
            if (at + 2 > std::size(s_))
                break;
            if (parameter.type == 0) {
                s_[at] = {Value::table, p};
            } else if (parameter.type == 1) {
                // root constants are not taken yet: left undefined, so a shader that reads
                // them is given up on
            } else {
                s_[at] = {Value::address_lo, p};
                s_[at + 1] = {Value::address_hi, p};
            }
        }
        if (stage_ == Stage::pixel)
            v_[0] = v_[1] = {Value::opaque};
        const auto &code = shader_.code;
        // the code starts after six words that are not instructions
        for (at_ = 6; at_ < code.size();) {
            if (!step())
                break;
        }
        return finish();
    }

  private:
    std::uint32_t next() {
        if (at_ >= shader_.code.size())
            throw Unsupported{};
        return shader_.code[at_++];
    }
    std::string temporary(const std::string &text) {
        const auto name = "t" + std::to_string(temporaries_++);
        body_ += "    uint " + name + " = " + text + ";\n";
        return name;
    }
    Value make(const std::string &text) { return expression(temporary(text)); }
    static std::string f(const std::string &bits) { return "asfloat(" + bits + ")"; }
    static std::string u(const std::string &number) { return "asuint(" + number + ")"; }
    std::string bits(const Value &value) {
        if (value.kind == Value::packed)
            return "(f32tof16(" + f(value.text) + ") | f32tof16(" + f(value.other) + ") << 16)";
        if (value.kind != Value::expression)
            throw Unsupported{};
        return value.text;
    }
    // an operand: a register, a small constant, or the literal that follows the instruction
    Value operand(std::uint32_t source, bool &literal) {
        if (source >= 256)
            return v_[source - 256];
        if (source < 106)
            return s_[source];
        if (source >= 128 && source <= 192)
            return constant(source - 128);
        if (source >= 193 && source <= 208)
            return constant(static_cast<std::uint32_t>(-static_cast<int>(source - 192)));
        static constexpr float floats[] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f};
        if (source >= 240 && source <= 247)
            return constant(std::bit_cast<std::uint32_t>(floats[source - 240]));
        if (source == 255) {
            literal = true;
            return {Value::expression, 0, 0, 0, "LITERAL", {}};
        }
        return {Value::opaque}; // exec, vcc, m0 and the rest
    }
    void resolve(Value &value, std::uint32_t literal) {
        if (value.kind == Value::expression && value.text == "LITERAL")
            value = constant(literal);
    }
    void set_s(std::uint32_t reg, Value value) {
        if (reg < 106)
            s_[reg] = std::move(value);
        // a write to exec, vcc or m0 is dropped: straight code has every lane on
    }

    // one instruction; false at the end of the program
    bool step() {
        const auto w = next();
        if (w >> 23 == 0b101111111) { // SOPP
            const auto op = (w >> 16) & 0x7f;
            if (op == 1)
                return false;
            if (op == 0 || op == 12 || op == 0x20 || op == 0x21 || op == 0x23)
                return true; // nop, waitcnt, prefetch, clause
            throw Unsupported{};
        }
        if (w >> 23 == 0b101111101) { // SOP1
            const auto op = (w >> 8) & 0xff, dst = (w >> 16) & 0x7f, src = w & 0xff;
            bool literal = false;
            auto value = operand(src, literal);
            if (literal)
                resolve(value, next());
            if (op == 3) {
                set_s(dst, value);
            } else if (op == 4) {
                set_s(dst, value);
                bool unused = false;
                set_s(dst + 1, operand(src + 1, unused));
            } else if (op == 9 || op == 10) { // whole quad mode: a mask
                set_s(dst, {Value::opaque});
                set_s(dst + 1, {Value::opaque});
            } else if (op == 0x21 && stage_ == Stage::vertex && src == 4) {
                fetch_inputs();
            } else if (op == 7 && value.kind == Value::expression) {
                set_s(dst, make("~" + value.text));
            } else {
                throw Unsupported{};
            }
            return true;
        }
        if (w >> 28 == 0b1011) { // SOPK
            const auto op = (w >> 23) & 0x1f, dst = (w >> 16) & 0x7f;
            if (op != 0)
                throw Unsupported{};
            set_s(dst, constant(static_cast<std::uint32_t>(static_cast<std::int16_t>(w & 0xffff))));
            return true;
        }
        if (w >> 23 == 0b101111110) // SOPC: compares set a condition, which nothing here reads
            throw Unsupported{};
        if (w >> 30 == 0b10) { // SOP2
            scalar_binary(w);
            return true;
        }
        const auto top = w >> 26;
        if (top == 0b110101) {
            vector_three(w);
        } else if (top == 0b110010) {
            interpolate(w);
        } else if (top == 0b111101) {
            scalar_load(w);
        } else if (top == 0b111100) {
            image(w);
        } else if (top == 0b111110) {
            export_values(w);
        } else if (w >> 25 == 0b0111111) {
            const auto op = (w >> 9) & 0xff, dst = (w >> 17) & 0xff;
            bool literal = false;
            auto a = operand(w & 0x1ff, literal);
            if (literal)
                resolve(a, next());
            v_[dst] = vector_one(op, a);
        } else if (w >> 31 == 0 && w >> 25 != 0b0111110) {
            const auto op = (w >> 25) & 0x3f, dst = (w >> 17) & 0xff;
            bool literal = false;
            auto a = operand(w & 0x1ff, literal);
            const auto b = v_[(w >> 9) & 0xff];
            const bool with_constant = op == 32 || op == 33 || op == 44 || op == 45;
            std::uint32_t value = 0;
            if (literal || with_constant)
                value = next();
            resolve(a, value);
            v_[dst] = vector_two(op, a, b, v_[dst], constant(value));
        } else {
            throw Unsupported{};
        }
        return true;
    }

    void scalar_binary(std::uint32_t w) {
        const auto op = (w >> 23) & 0x7f, dst = (w >> 16) & 0x7f;
        bool literal = false;
        auto a = operand(w & 0xff, literal), b = operand((w >> 8) & 0xff, literal);
        if (literal) {
            const auto value = next();
            resolve(a, value);
            resolve(b, value);
        }
        // the high half of an address with bits set in it is still that half
        if (op == 16 && a.kind == Value::address_hi && b.kind == Value::expression) {
            set_s(dst, a);
            return;
        }
        // 64-bit mask arithmetic, and anything on a mask
        if (a.kind == Value::opaque || b.kind == Value::opaque || op == 15 || op == 17 ||
            op == 19 || op == 21) {
            set_s(dst, {Value::opaque});
            if (op == 15 || op == 17 || op == 19 || op == 21)
                set_s(dst + 1, {Value::opaque});
            return;
        }
        const auto x = bits(a), y = bits(b);
        const auto i = [](const std::string &t) { return "(int)" + t; };
        std::string text;
        switch (op) {
        case 0: case 2: text = x + " + " + y; break;
        case 1: case 3: text = x + " - " + y; break;
        case 6: text = u("min(" + i(x) + ", " + i(y) + ")"); break;
        case 7: text = "min(" + x + ", " + y + ")"; break;
        case 8: text = u("max(" + i(x) + ", " + i(y) + ")"); break;
        case 9: text = "max(" + x + ", " + y + ")"; break;
        case 14: text = x + " & " + y; break;
        case 16: text = x + " | " + y; break;
        case 18: text = x + " ^ " + y; break;
        case 20: text = x + " & ~" + y; break;
        case 30: text = x + " << (" + y + " & 31u)"; break;
        case 32: text = x + " >> (" + y + " & 31u)"; break;
        case 34: text = u(i(x) + " >> (" + y + " & 31u)"); break;
        case 38: text = x + " * " + y; break;
        default: throw Unsupported{};
        }
        set_s(dst, make(text));
    }

    Value vector_one(std::uint32_t op, const Value &a) {
        if (op == 1)
            return a;
        const auto x = bits(a);
        switch (op) {
        case 5: return make(u("(float)(int)" + x));
        case 6: return make(u("(float)" + x));
        case 7: return make("(uint)" + f(x));
        case 8: return make(u("(int)" + f(x)));
        case 32: return make(u("frac(" + f(x) + ")"));
        case 33: return make(u("trunc(" + f(x) + ")"));
        case 34: return make(u("ceil(" + f(x) + ")"));
        case 35: return make(u("round(" + f(x) + ")"));
        case 36: return make(u("floor(" + f(x) + ")"));
        case 37: return make(u("exp2(" + f(x) + ")"));
        case 39: return make(u("log2(" + f(x) + ")"));
        case 42: case 43: return make(u("1.0 / " + f(x)));
        case 46: return make(u("rsqrt(" + f(x) + ")"));
        case 51: return make(u("sqrt(" + f(x) + ")"));
        case 53: return make(u("sin(6.28318530718 * " + f(x) + ")"));
        case 54: return make(u("cos(6.28318530718 * " + f(x) + ")"));
        case 55: return make("~" + x);
        default: throw Unsupported{};
        }
    }
    // `a` is the first source and `b` the second; `old` what the destination had
    Value vector_two(std::uint32_t op, const Value &a, const Value &b, const Value &old,
                     const Value &k) {
        if (op == 0x2f) // two floats packed as halves
            return {Value::packed, 0, 0, 0, bits(a), bits(b)};
        const auto x = bits(a), y = bits(b);
        const auto i = [](const std::string &t) { return "(int)" + t; };
        switch (op) {
        case 3: return make(u(f(x) + " + " + f(y)));
        case 4: return make(u(f(x) + " - " + f(y)));
        case 5: return make(u(f(y) + " - " + f(x)));
        case 7: case 8: return make(u(f(x) + " * " + f(y)));
        case 15: return make(u("min(" + f(x) + ", " + f(y) + ")"));
        case 16: return make(u("max(" + f(x) + ", " + f(y) + ")"));
        case 17: return make(u("min(" + i(x) + ", " + i(y) + ")"));
        case 18: return make(u("max(" + i(x) + ", " + i(y) + ")"));
        case 19: return make("min(" + x + ", " + y + ")");
        case 20: return make("max(" + x + ", " + y + ")");
        case 22: return make(y + " >> (" + x + " & 31u)");
        case 24: return make(u(i(y) + " >> (" + x + " & 31u)"));
        case 26: return make(y + " << (" + x + " & 31u)");
        case 27: return make(x + " & " + y);
        case 28: return make(x + " | " + y);
        case 29: return make(x + " ^ " + y);
        case 31: case 43: return make(u(f(x) + " * " + f(y) + " + " + f(bits(old))));
        case 32: case 44: return make(u(f(x) + " * " + f(bits(k)) + " + " + f(y)));
        case 33: case 45: return make(u(f(x) + " * " + f(y) + " + " + f(bits(k))));
        case 37: return make(x + " + " + y);
        case 38: return make(x + " - " + y);
        case 39: return make(y + " - " + x);
        default: throw Unsupported{};
        }
    }
    void vector_three(std::uint32_t w) {
        const auto op = (w >> 16) & 0x3ff, dst = w & 0xff;
        const auto x = next();
        bool literal = false;
        Value source[3] = {operand(x & 0x1ff, literal), operand((x >> 9) & 0x1ff, literal),
                           operand((x >> 18) & 0x1ff, literal)};
        if (literal) {
            const auto value = next();
            for (auto &one : source)
                resolve(one, value);
        }
        // the sign and the magnitude of the float sources
        const auto absolute = (w >> 8) & 7, negated = x >> 29;
        for (int n = 0; n < 3; ++n) {
            if (!((absolute | negated) >> n & 1) || source[n].kind == Value::undefined)
                continue;
            auto text = f(bits(source[n]));
            if (absolute >> n & 1)
                text = "abs(" + text + ")";
            if (negated >> n & 1)
                text = "-" + text;
            source[n] = make(u(text));
        }
        Value result;
        if (op >= 0x100 && op < 0x140) {
            result = vector_two(op - 0x100, source[0], source[1], v_[dst], source[2]);
        } else if (op >= 0x180 && op < 0x1c0) {
            result = vector_one(op - 0x180, source[0]);
        } else {
            const auto a = bits(source[0]), b = bits(source[1]), c = bits(source[2]);
            switch (op) {
            case 0x140: case 0x141: case 0x14b:
                result = make(u(f(a) + " * " + f(b) + " + " + f(c)));
                break;
            case 0x148: result = make("(" + a + " >> (" + b + " & 31u)) & ((1u << (" + c + " & 31u)) - 1u)"); break;
            case 0x151: result = make(u("min(min(" + f(a) + ", " + f(b) + "), " + f(c) + ")")); break;
            case 0x154: result = make(u("max(max(" + f(a) + ", " + f(b) + "), " + f(c) + ")")); break;
            case 0x157:
                result = make(u("max(min(" + f(a) + ", " + f(b) + "), min(max(" + f(a) + ", " + f(b) + "), " + f(c) + "))"));
                break;
            case 0x169: case 0x16b: result = make(a + " * " + b); break;
            case 0x346: result = make("(" + a + " << (" + b + " & 31u)) + " + c); break;
            case 0x347: result = make("(" + a + " + " + b + ") << (" + c + " & 31u)"); break;
            case 0x36d: result = make(a + " + " + b + " + " + c); break;
            case 0x36f: result = make("(" + a + " & " + b + ") | " + c); break;
            case 0x370: result = make(a + " | " + b + " | " + c); break;
            default: throw Unsupported{};
            }
        }
        const auto scale = (x >> 27) & 3;
        if (scale || (w >> 15 & 1)) {
            auto text = f(bits(result));
            if (scale)
                text += scale == 1 ? " * 2.0" : scale == 2 ? " * 4.0" : " * 0.5";
            if (w >> 15 & 1)
                text = "saturate(" + text + ")";
            result = make(u(text));
        }
        v_[dst] = result;
    }

    // the input elements that are not system values, in register order: attribute numbers
    std::vector<std::size_t> attributes(const std::vector<Element> &elements) {
        std::vector<std::size_t> found;
        for (std::size_t i = 0; i < elements.size(); ++i)
            if (!elements[i].system_value)
                found.push_back(i);
        return found;
    }
    // channel `channel` of field `field` of the input, as bits; what the element lacks reads 0
    std::string input_channel(const Element &element, std::size_t field, std::uint32_t channel,
                              std::uint32_t missing) {
        if (channel >= element.width)
            return std::to_string(missing) + "u";
        const auto name = "input.f" + std::to_string(field) +
                          (element.width > 1 ? std::string(".") + "xyzw"[channel] : "");
        return element.type == 3 ? u(name) : "(uint)" + name;
    }
    void fetch_inputs() {
        const auto &inputs = shader_.inputs;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            if (inputs[i].system_value || 4 + 4 * i + 3 >= std::size(v_))
                throw Unsupported{};
            for (std::uint32_t channel = 0; channel < 4; ++channel)
                v_[4 + 4 * i + channel] = make(input_channel(
                    inputs[i], i, channel, channel == 3 ? std::bit_cast<std::uint32_t>(1.0f) : 0));
        }
        fetched_ = true;
    }
    void interpolate(std::uint32_t w) {
        if (stage_ != Stage::pixel)
            throw Unsupported{};
        const auto dst = (w >> 18) & 0xff, attribute = (w >> 10) & 0x3f, channel = (w >> 8) & 3;
        const auto numbers = attributes(shader_.inputs);
        if (attribute >= numbers.size())
            throw Unsupported{};
        const auto field = numbers[attribute];
        // the two steps of an interpolation both leave the interpolated value
        v_[dst] = make(input_channel(shader_.inputs[field], field, channel, 0));
    }

    void scalar_load(std::uint32_t w) {
        const auto op = (w >> 18) & 0xff, dst = (w >> 6) & 0x7f, base = (w & 0x3f) * 2;
        const auto x = next();
        const auto offset = x & 0x1fffff, index_register = x >> 25;
        const bool indexed = index_register != 0x7d;
        if (base + 3 >= std::size(s_))
            throw Unsupported{};
        if (op <= 4) { // from memory: a descriptor out of a table
            const std::uint32_t words = 1u << op;
            if (s_[base].kind != Value::table || (words != 4 && words != 8))
                throw Unsupported{};
            const std::uint32_t size = words * 4;
            std::string index = std::to_string(offset / size) + "u";
            if (indexed)
                index = temporary("(" + bits(s_[index_register]) + " + " + std::to_string(offset) +
                                  "u) / " + std::to_string(size) + "u");
            for (std::uint32_t n = 0; n < words; ++n)
                set_s(dst + n, {Value::descriptor, s_[base].a, 0, n, index, indexed ? "dynamic" : ""});
            return;
        }
        if (op < 8 || op > 12 || indexed)
            throw Unsupported{};
        // from a buffer: a constant buffer, by a descriptor of a table or by a root address
        std::string buffer;
        if (s_[base].kind == Value::descriptor && s_[base].c == 0 && s_[base].other.empty())
            buffer = constant_buffer(s_[base].a, std::stoul(s_[base].text));
        else if (s_[base].kind == Value::address_lo && s_[base + 1].kind == Value::address_hi &&
                 s_[base].a == s_[base + 1].a && parameters_[s_[base].a].type == 2)
            buffer = constant_buffer(s_[base].a, ~0u);
        else
            throw Unsupported{};
        for (std::uint32_t n = 0; n < (1u << (op - 8)); ++n) {
            const auto at = offset / 4 + n;
            set_s(dst + n, make(buffer + "[" + std::to_string(at / 4) + "]." + "xyzw"[at % 4]));
        }
    }
    // the register and space of descriptor `index` of table `parameter`, for a range of
    // type `type`
    const Range &range_of(std::uint32_t parameter, std::uint32_t type, std::uint32_t index) {
        for (const auto &range : parameters_[parameter].ranges)
            if (range.type == type && index >= range.offset &&
                (range.count == ~0u || index - range.offset < range.count))
                return range;
        throw Unsupported{};
    }
    std::string constant_buffer(std::uint32_t parameter, std::uint32_t index) {
        const auto name = "c" + std::to_string(parameter) +
                          (index == ~0u ? std::string() : "_" + std::to_string(index));
        if (declared_.insert(name).second) {
            std::uint32_t reg, space;
            if (index == ~0u) {
                reg = parameters_[parameter].reg;
                space = parameters_[parameter].space;
            } else {
                const auto &range = range_of(parameter, 2, index);
                reg = range.base + (index - range.offset);
                space = range.space;
            }
            declarations_ += "cbuffer " + name + "_buffer : register(b" + std::to_string(reg) +
                             ", space" + std::to_string(space) + ") { uint4 " + name +
                             "[4096]; };\n";
        }
        return name;
    }

    void image(std::uint32_t w) {
        const auto op = (w >> 18) & 0x7f | (w & 1) << 7;
        const auto x = next();
        if ((w >> 1) & 3) // addresses spread over registers
            throw Unsupported{};
        const auto data = (x >> 8) & 0xff, address = x & 0xff;
        const auto resource = ((x >> 16) & 0x1f) * 4, sampler = ((x >> 21) & 0x1f) * 4;
        const auto mask = (w >> 8) & 0xf, dimension = (w >> 3) & 7;
        const auto &texture = s_[resource], &state = s_[sampler];
        if (texture.kind != Value::descriptor || texture.c != 0 || state.kind != Value::descriptor ||
            state.c != 0 || !state.other.empty())
            throw Unsupported{};
        // 1: 2D, 2: 3D, 5: 2D array
        const char *type;
        int coordinates;
        if (dimension == 1)
            type = "Texture2D<float4>", coordinates = 2;
        else if (dimension == 2)
            type = "Texture3D<float4>", coordinates = 3;
        else if (dimension == 5)
            type = "Texture2DArray<float4>", coordinates = 3;
        else
            throw Unsupported{};
        auto first = address;
        std::string extra, method;
        if (op == 0x20 && stage_ == Stage::pixel) {
            method = "Sample";
        } else if (op == 0x27) {
            method = "SampleLevel";
            extra = ", 0.0";
        } else if (op == 0x24) {
            method = "SampleLevel";
            extra = ", " + f(bits(v_[address + coordinates]));
        } else if (op == 0x25 && stage_ == Stage::pixel) {
            method = "SampleBias";
            extra = ", " + f(bits(v_[address]));
            first = address + 1;
        } else {
            throw Unsupported{};
        }
        std::string where = "float" + std::to_string(coordinates) + "(";
        for (int n = 0; n < coordinates; ++n)
            where += (n ? ", " : "") + f(bits(v_[first + n]));
        where += ")";

        // the sampler: one of a table, by its place there
        const auto sampler_index = static_cast<std::uint32_t>(std::stoul(state.text));
        const auto sampler_name = "s" + std::to_string(state.a) + "_" + std::to_string(sampler_index);
        if (declared_.insert(sampler_name).second) {
            const auto &range = range_of(state.a, 3, sampler_index);
            declarations_ += "SamplerState " + sampler_name + " : register(s" +
                             std::to_string(range.base + (sampler_index - range.offset)) +
                             ", space" + std::to_string(range.space) + ");\n";
        }
        // the texture: one of a table by its place, or any of a table by a computed index
        std::string texture_name;
        if (texture.other.empty()) {
            const auto index = static_cast<std::uint32_t>(std::stoul(texture.text));
            texture_name = "r" + std::to_string(texture.a) + "_" + std::to_string(index);
            if (declared_.insert(texture_name).second) {
                const auto &range = range_of(texture.a, 0, index);
                declarations_ += std::string(type) + " " + texture_name + " : register(t" +
                                 std::to_string(range.base + (index - range.offset)) + ", space" +
                                 std::to_string(range.space) + ");\n";
            }
        } else {
            // every range of the table that starts at its first descriptor names the same
            // descriptors: one of them is declared with each type of texture that is used
            const auto key = "r" + std::to_string(texture.a) + "_d" + std::to_string(dimension);
            if (declared_.insert(key).second) {
                const Range *free = nullptr;
                for (const auto &range : parameters_[texture.a].ranges)
                    if (range.type == 0 && range.offset == 0 && range.count == ~0u &&
                        used_ranges_.insert({texture.a, range.space}).second) {
                        free = &range;
                        break;
                    }
                if (!free)
                    throw Unsupported{};
                declarations_ += std::string(type) + " " + key + "[] : register(t" +
                                 std::to_string(free->base) + ", space" +
                                 std::to_string(free->space) + ");\n";
            }
            texture_name = key + "[" + texture.text + "]";
        }
        const auto sample = "x" + std::to_string(temporaries_++);
        body_ += "    float4 " + sample + " = " + texture_name + "." + method + "(" + sampler_name +
                 ", " + where + extra + ");\n";
        std::uint32_t at = data;
        for (int channel = 0; channel < 4; ++channel)
            if (mask >> channel & 1)
                v_[at++] = make(u(sample + "." + "xyzw"[channel]));
    }

    void export_values(std::uint32_t w) {
        const auto x = next();
        const auto target = (w >> 4) & 0x3f, enabled = w & 0xf;
        const bool compressed = w >> 10 & 1;
        std::string channels[4];
        for (int n = 0; n < 4; ++n)
            channels[n] = "0.0";
        if (compressed) {
            // two registers, each two halves
            for (int pair = 0; pair < 2; ++pair) {
                if (!(enabled >> (2 * pair) & 3))
                    continue;
                const auto &value = v_[(x >> (8 * pair)) & 0xff];
                if (value.kind == Value::packed) {
                    channels[2 * pair] = f(value.text);
                    channels[2 * pair + 1] = f(value.other);
                } else {
                    const auto both = bits(value);
                    channels[2 * pair] = "f16tof32(" + both + ")";
                    channels[2 * pair + 1] = "f16tof32(" + both + " >> 16)";
                }
            }
        } else {
            for (int n = 0; n < 4; ++n)
                if (enabled >> n & 1)
                    channels[n] = f(bits(v_[(x >> (8 * n)) & 0xff]));
        }
        const auto &outputs = shader_.outputs;
        std::size_t field = outputs.size();
        if (stage_ == Stage::vertex && target == 12) {
            for (std::size_t i = 0; i < outputs.size(); ++i)
                if (!_stricmp(outputs[i].name.c_str(), "SV_Position"))
                    field = i;
        } else if (stage_ == Stage::vertex && target >= 32) {
            const auto numbers = attributes(outputs);
            if (target - 32 < numbers.size())
                field = numbers[target - 32];
        } else if (stage_ == Stage::pixel && target < 8) {
            for (std::size_t i = 0; i < outputs.size(); ++i)
                if (!_stricmp(outputs[i].name.c_str(), "SV_Target") && outputs[i].index == target)
                    field = i;
        }
        if (field == outputs.size())
            throw Unsupported{};
        const auto &element = outputs[field];
        std::string value = element.type == 3 ? "float4(" : "asuint(float4(";
        for (int n = 0; n < 4; ++n)
            value += (n ? ", " : "") + channels[n];
        value += element.type == 3 ? ")" : "))";
        if (element.type == 2)
            value = "(int4)" + value;
        static constexpr const char *swizzle[] = {"x", "x", "xy", "xyz", "xyzw"};
        body_ += "    output.f" + std::to_string(field) + " = " + value + "." +
                 swizzle[std::min<std::uint32_t>(element.width, 4)] + ";\n";
        exported_ = true;
    }

    std::string finish() {
        if (!exported_ || (stage_ == Stage::vertex && !fetched_ && !shader_.inputs.empty()))
            throw Unsupported{};
        return declarations_ + "Output main(Input input) {\n    Output output = (Output)0;\n" +
               body_ + "    return output;\n}\n";
    }

    Stage stage_;
    const Shader &shader_;
    std::vector<Parameter> parameters_;
    Value s_[108], v_[256];
    std::size_t at_ = 0;
    int temporaries_ = 0;
    bool fetched_ = false, exported_ = false;
    std::string declarations_, body_;
    std::set<std::string> declared_;
    std::set<std::pair<std::uint32_t, std::uint32_t>> used_ranges_;
};
} // namespace

std::optional<std::string> translate(Stage stage, const Shader &shader) {
    if (stage == Stage::compute || shader.code.size() < 8)
        return std::nullopt;
    try {
        // not on the stack: it is large, and a title's threads may have little
        return std::make_unique<Translator>(stage, shader)->run();
    } catch (const Unsupported &) {
        return std::nullopt;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}
} // namespace xs::runtime::modules::rdna
