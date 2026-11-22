#include "runtime/modules/console_shader.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/console_shader_rdna.hpp"
#include "runtime/modules/inflate.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <set>
#include <thread>
#include <vector>
#include <string>
#include <windows.h>
#include <xmmintrin.h>

#include <unknwn.h>

namespace xv::runtime::modules {
namespace {
// Bumped whenever what is made for a shader changes: the cache keeps one file per version.
constexpr int translator_version = 11;

using Bytes = std::span<const std::uint8_t>;
std::uint32_t word(Bytes bytes, std::size_t at) {
    std::uint32_t value = 0;
    if (at + 4 <= bytes.size())
        std::memcpy(&value, bytes.data() + at, 4);
    return value;
}
// a part of a DXBC container, by its four letters; empty if it has none
Bytes part(Bytes container, const char *name) {
    if (container.size() < 32 || std::memcmp(container.data(), "DXBC", 4))
        return {};
    const auto count = word(container, 28);
    for (std::uint32_t i = 0; i < count && 36 + 4 * i <= container.size(); ++i) {
        const auto offset = word(container, 32 + 4 * i);
        if (offset + 8 > container.size() || std::memcmp(container.data() + offset, name, 4))
            continue;
        const auto size = word(container, offset + 4);
        if (size > container.size() - offset - 8)
            return {};
        return container.subspan(offset + 8, size);
    }
    return {};
}

// Inflate (RFC 1951) of a zlib stream (RFC 1950), which is how the code is stored
class Inflate {
  public:
    explicit Inflate(Bytes input) : in_(input) {}
    bool run(std::vector<std::uint8_t> &out) {
        if (in_.size() < 2 || (in_[0] & 0x0f) != 8)
            return false;
        at_ = 2;
        for (bool last = false; !last;) {
            last = bits(1);
            const auto type = bits(2);
            if (type == 0) {
                count_ = 0;
                if (at_ + 4 > in_.size())
                    return false;
                const std::size_t length = in_[at_] | in_[at_ + 1] << 8;
                at_ += 4;
                if (at_ + length > in_.size())
                    return false;
                out.insert(out.end(), in_.begin() + at_, in_.begin() + at_ + length);
                at_ += length;
            } else if (type == 1) {
                fixed();
                if (!codes(out))
                    return false;
            } else if (type == 2) {
                if (!dynamic() || !codes(out))
                    return false;
            } else {
                return false;
            }
            if (failed_ || out.size() > 64u << 20)
                return false;
        }
        return true;
    }

  private:
    struct Table {
        std::array<std::uint16_t, 16> count{};
        std::array<std::uint16_t, 288> symbol{};
    };
    std::uint32_t bits(int need) {
        while (count_ < need) {
            if (at_ >= in_.size()) {
                failed_ = true;
                return 0;
            }
            hold_ |= std::uint32_t{in_[at_++]} << count_;
            count_ += 8;
        }
        const auto value = hold_ & ((1u << need) - 1);
        hold_ >>= need;
        count_ -= need;
        return value;
    }
    static void build(Table &table, const std::uint8_t *lengths, int n) {
        table.count.fill(0);
        for (int i = 0; i < n; ++i)
            ++table.count[lengths[i]];
        table.count[0] = 0;
        std::array<std::uint16_t, 16> offsets{};
        for (int length = 1; length < 16; ++length)
            offsets[length] = offsets[length - 1] + table.count[length - 1];
        for (int i = 0; i < n; ++i)
            if (lengths[i])
                table.symbol[offsets[lengths[i]]++] = static_cast<std::uint16_t>(i);
    }
    int decode(const Table &table) {
        int code = 0, first = 0, index = 0;
        for (int length = 1; length < 16; ++length) {
            code |= static_cast<int>(bits(1));
            const int count = table.count[length];
            if (code - count < first)
                return table.symbol[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        failed_ = true;
        return -1;
    }
    void fixed() {
        std::uint8_t lengths[288];
        for (int i = 0; i < 288; ++i)
            lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        build(literals_, lengths, 288);
        std::fill(lengths, lengths + 30, std::uint8_t{5});
        build(distances_, lengths, 30);
    }
    bool dynamic() {
        const int literal_count = static_cast<int>(bits(5)) + 257;
        const int distance_count = static_cast<int>(bits(5)) + 1;
        const int length_count = static_cast<int>(bits(4)) + 4;
        static constexpr std::uint8_t order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                   11, 4,  12, 3, 13, 2, 14, 1, 15};
        std::uint8_t lengths[320] = {};
        for (int i = 0; i < length_count; ++i)
            lengths[order[i]] = static_cast<std::uint8_t>(bits(3));
        Table table;
        build(table, lengths, 19);
        std::fill(lengths, lengths + 320, std::uint8_t{0});
        for (int i = 0; i < literal_count + distance_count;) {
            const int symbol = decode(table);
            if (symbol < 0)
                return false;
            if (symbol < 16) {
                lengths[i++] = static_cast<std::uint8_t>(symbol);
                continue;
            }
            std::uint8_t value = 0;
            int repeat;
            if (symbol == 16) {
                if (!i)
                    return false;
                value = lengths[i - 1];
                repeat = 3 + static_cast<int>(bits(2));
            } else {
                repeat = symbol == 17 ? 3 + static_cast<int>(bits(3)) : 11 + static_cast<int>(bits(7));
            }
            if (i + repeat > literal_count + distance_count)
                return false;
            while (repeat--)
                lengths[i++] = value;
        }
        build(literals_, lengths, literal_count);
        build(distances_, lengths + literal_count, distance_count);
        return !failed_;
    }
    bool codes(std::vector<std::uint8_t> &out) {
        static constexpr std::uint16_t length_base[29] = {
            3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
            31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr std::uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                                          1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                                          4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr std::uint16_t distance_base[30] = {
            1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
            193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr std::uint8_t distance_extra[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                                            4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                                            9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            const int symbol = decode(literals_);
            if (symbol < 0 || failed_)
                return false;
            if (symbol < 256) {
                out.push_back(static_cast<std::uint8_t>(symbol));
            } else if (symbol == 256) {
                return true;
            } else {
                if (symbol > 285)
                    return false;
                const std::size_t length =
                    length_base[symbol - 257] + bits(length_extra[symbol - 257]);
                const int code = decode(distances_);
                if (code < 0 || code > 29)
                    return false;
                const std::size_t distance = distance_base[code] + bits(distance_extra[code]);
                if (distance > out.size())
                    return false;
                for (std::size_t i = 0; i < length; ++i)
                    out.push_back(out[out.size() - distance]);
            }
        }
    }
    Bytes in_;
    std::size_t at_ = 0;
    std::uint32_t hold_ = 0;
    int count_ = 0;
    bool failed_ = false;
    Table literals_, distances_;
};

// The XDAT block of the DXIL part: after the program header, a table of sections as (type,
// offset, size). Section 0x0d is the zlib stream with the code.
std::vector<std::uint8_t> code_container(Bytes container) {
    const auto dxil = part(container, "DXIL");
    constexpr std::size_t xdat = 24;
    if (dxil.size() < xdat + 16 || std::memcmp(dxil.data() + xdat, "XDAT", 4))
        return {};
    const auto block = dxil.subspan(xdat);
    for (std::size_t entry = 12; entry + 12 <= std::min<std::size_t>(block.size(), 12 + 16 * 12);
         entry += 12) {
        const auto type = word(block, entry), offset = word(block, entry + 4),
                   size = word(block, entry + 8);
        if (!type || type > 0xff || offset > block.size() || size > block.size() - offset)
            break;
        if (type != 0x0d)
            continue;
        std::vector<std::uint8_t> out;
        if (Inflate(block.subspan(offset, size)).run(out) && out.size() >= 16 &&
            !std::memcmp(out.data(), "FSSM", 4))
            return out;
        return {};
    }
    return {};
}

using rdna::Element;
// an ISG1 or OSG1 signature, in the order of its registers
std::vector<Element> signature(Bytes container, const char *name) {
    const auto data = part(container, name);
    struct Read {
        Element element;
        std::uint32_t reg;
        std::uint8_t mask;
    };
    std::vector<Read> read;
    const auto count = word(data, 0);
    for (std::uint32_t i = 0; i < count && i < 64; ++i) {
        const std::size_t at = 8 + 32 * std::size_t{i};
        if (at + 32 > data.size())
            return {};
        const auto text = word(data, at + 4);
        if (text >= data.size())
            return {};
        const auto *const start = reinterpret_cast<const char *>(data.data()) + text;
        Element element;
        element.name.assign(start, strnlen(start, data.size() - text));
        element.index = word(data, at + 8);
        element.type = word(data, at + 16);
        element.width = static_cast<std::uint32_t>(std::max(std::popcount(data[at + 24]), 1));
        element.system_value =
            word(data, at + 12) != 0 || !_strnicmp(element.name.c_str(), "SV_", 3);
        element.reg = word(data, at + 20);
        element.mask = data[at + 24];
        element.read = data[at + 25];
        read.push_back({std::move(element), word(data, at + 20), data[at + 24]});
    }
    std::stable_sort(read.begin(), read.end(), [](const Read &a, const Read &b) {
        return a.reg != b.reg ? a.reg < b.reg : a.mask < b.mask;
    });
    std::vector<Element> elements;
    for (auto &one : read)
        elements.push_back(std::move(one.element));
    return elements;
}
// The fields of a signature as an HLSL struct: element n is the field fn. The values the
// pipeline makes up itself are declared after everything else, as the compiler wants.
std::string hlsl_struct(const char *name, const std::vector<Element> &elements,
                        bool interpolated) {
    const auto generated = [](const Element &element) {
        for (const auto *made : {"SV_VertexID", "SV_InstanceID", "SV_PrimitiveID",
                                 "SV_IsFrontFace", "SV_SampleIndex", "SV_Coverage"})
            if (!_stricmp(element.name.c_str(), made))
                return true;
        return false;
    };
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
            text += std::string(element.type == 1 ? "uint" : element.type == 2 ? "int" : "float") +
                    (element.width > 1 ? std::to_string(element.width) : "") + " f" +
                    std::to_string(field) + " : " + element.name;
            // SV_Position and the like take no index; the others always may
            if (!named || element.index || !_stricmp(element.name.c_str(), "SV_Target") ||
                !_stricmp(element.name.c_str(), "SV_ClipDistance") ||
                !_stricmp(element.name.c_str(), "SV_CullDistance"))
                text += std::to_string(element.index);
            text += ";\n";
        }
    return text + "};\n";
}

// A stage without inputs takes none: `main` has no parameter, and no struct is made. (A
// made-up input would have to match what the stage before writes.)
std::string without_input(std::string source) {
    const std::string parameter = "main(Input input)";
    if (const auto at = source.find(parameter); at != std::string::npos)
        source.replace(at, parameter.size(), "main()");
    return source;
}
// A shader with the original's signature that does nothing: a vertex shader that puts every
// vertex at the origin (so nothing is drawn), a pixel shader that writes zero, an empty
// compute shader. It stands in for a shader that is not translated, so that the title's
// pipelines exist and it goes on.
std::string stand_in(ShaderStage stage, Bytes container) {
    if (stage == ShaderStage::compute)
        return "[numthreads(1, 1, 1)] void main() {}\n";
    const auto inputs = signature(container, "ISG1"), outputs = signature(container, "OSG1");
    // a geometry shader that draws nothing
    if (stage >= ShaderStage::geometry_points) {
        if (inputs.empty() || outputs.empty())
            return {};
        const char *const primitive = stage == ShaderStage::geometry_points  ? "point Input input[1]"
                                      : stage == ShaderStage::geometry_lines ? "line Input input[2]"
                                                                             : "triangle Input input[3]";
        return hlsl_struct("Input", inputs, false) + hlsl_struct("Output", outputs, false) +
               "[maxvertexcount(1)] void main(" + primitive +
               ", inout TriangleStream<Output> stream) {\n"
               // one vertex makes no triangle, and gives the shader its output signature
               "    Output output = (Output)0;\n    stream.Append(output);\n}\n";
    }
    std::string text =
        inputs.empty() ? "" : hlsl_struct("Input", inputs, stage == ShaderStage::pixel);
    if (outputs.empty())
        text += "void main(Input input) {}\n";
    else
        text += hlsl_struct("Output", outputs, false) +
                "Output main(Input input) {\n    Output output = (Output)0;\n    return output;\n}\n";
    return inputs.empty() ? without_input(std::move(text)) : text;
}
// the translation of the code; nothing if the code is not one that is handled, and then
// `why` says what stopped it
std::optional<std::string> translated_source(ShaderStage stage, Bytes container,
                                             std::string *why = nullptr) {
    if (stage != ShaderStage::vertex && stage != ShaderStage::pixel && stage != ShaderStage::compute)
        return std::nullopt;
    const auto file = code_container(container);
    rdna::Shader shader;
    shader.file = file;
    shader.root_signature = part(container, "RTS0");
    shader.inputs = signature(container, "ISG1");
    shader.outputs = signature(container, "OSG1");
    return rdna::translate(stage, shader, why);
}

using Compile = HRESULT(WINAPI *)(const void *, SIZE_T, const char *, const void *, void *,
                                  const char *, const char *, UINT, UINT, IUnknown **,
                                  IUnknown **);
struct Blob : IUnknown {
    virtual void *STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
};
std::vector<std::uint8_t> compile_now(ShaderStage stage, const std::string &source, bool report) {
    static const auto function = [] {
        const auto library =
            LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return library ? reinterpret_cast<Compile>(GetProcAddress(library, "D3DCompile")) : nullptr;
    }();
    if (!function)
        return {};
    if (source.empty())
        return {};
    const char *const target = stage == ShaderStage::vertex    ? "vs_5_1"
                               : stage == ShaderStage::pixel   ? "ps_5_1"
                               : stage == ShaderStage::compute ? "cs_5_1"
                                                               : "gs_5_1";
    constexpr UINT unbounded_tables = 1u << 20; // D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES
    IUnknown *code = nullptr, *errors = nullptr;
    // the compiler is run with the floating point state a host program has, not the
    // title's thread's
    const auto title_state = _mm_getcsr();
    _mm_setcsr(0x1f80);
    const auto result = function(source.data(), source.size(), "console shader", nullptr, nullptr,
                                 "main", target, unbounded_tables, 0, &code, &errors);
    _mm_setcsr(title_state);
    std::vector<std::uint8_t> bytes;
    if (SUCCEEDED(result) && code) {
        auto *const blob = static_cast<Blob *>(code);
        const auto *const data = static_cast<const std::uint8_t *>(blob->GetBufferPointer());
        bytes.assign(data, data + blob->GetBufferSize());
    } else if (static std::atomic<int> shown{0}; report && shown++ < 4) {
        std::fprintf(stderr, "[Runtime] a host shader for a console one did not compile: %s\n%s\n",
                     errors ? static_cast<const char *>(static_cast<Blob *>(errors)->GetBufferPointer())
                            : "no message",
                     source.c_str());
        std::fflush(stderr);
    }
    if (code)
        code->Release();
    if (errors)
        errors->Release();
    return bytes;
}
void write_file(const std::filesystem::path &path, Bytes bytes);
// The host's compiler takes minutes over some of the largest shaders, and the title waits
// for each of its shaders. So it is given `seconds`: a shader that takes longer is left to
// finish on its own and goes to `keep`, the shader cache, for the next run; this run gets
// nothing for it. XV_SHADER_SECONDS changes how long that is.
std::vector<std::uint8_t> compile(ShaderStage stage, const std::string &source,
                                  const std::filesystem::path &keep = {}) {
    static const int seconds = [] {
        const auto *const text = std::getenv("XV_SHADER_SECONDS");
        return text ? std::max(std::atoi(text), 1) : 20;
    }();
    struct Job {
        std::mutex mutex;
        std::condition_variable finished;
        bool done = false, abandoned = false;
        std::vector<std::uint8_t> bytes;
    };
    const auto job = std::make_shared<Job>();
    std::thread([job, stage, source, keep] {
        auto bytes = compile_now(stage, source, true);
        const std::lock_guard lock(job->mutex);
        if (job->abandoned && !bytes.empty() && !keep.empty())
            write_file(keep, bytes);
        job->bytes = std::move(bytes);
        job->done = true;
        job->finished.notify_all();
    }).detach();
    std::unique_lock lock(job->mutex);
    if (job->finished.wait_for(lock, std::chrono::seconds(seconds), [&] { return job->done; }))
        return std::move(job->bytes);
    job->abandoned = true;
    static std::atomic<int> shown{0};
    if (shown++ < 4)
        std::fprintf(stderr,
                     "[Runtime] the host's compiler has had %d s for a console shader of %zu "
                     "characters: it draws nothing in this run%s\n",
                     seconds, source.size(), keep.empty() ? "" : ", and is kept for the next");
    return {};
}

std::string hash_name(Bytes bytes) {
    std::uint64_t a = 0xcbf29ce484222325ull, b = 0x84222325cbf29ce4ull;
    for (const auto byte : bytes) {
        a = (a ^ byte) * 0x100000001b3ull;
        b = (b ^ byte) * 0x100000001b3ull + 0x9e3779b97f4a7c15ull;
    }
    char text[40];
    std::snprintf(text, sizeof text, "%016llx%016llx", static_cast<unsigned long long>(a),
                  static_cast<unsigned long long>(b));
    return text;
}
std::vector<std::uint8_t> read_file(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void write_file(const std::filesystem::path &path, Bytes bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    // written whole or not at all: another title process may be writing the same shader
    auto partial = path;
    partial += "." + std::to_string(GetCurrentProcessId());
    {
        std::ofstream file(partial, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    std::filesystem::rename(partial, path, error);
    if (error)
        std::filesystem::remove(partial, error);
}

std::mutex mutex;
std::condition_variable made_one;
std::map<std::string, std::vector<std::uint8_t>> made; // by cache name; never erased
std::set<std::string> making;                          // by cache name, on some thread
std::atomic<unsigned> translated{0}, stood_in{0};
} // namespace

bool inflate_zlib(Bytes stream, std::vector<std::uint8_t> &out) {
    return Inflate(stream).run(out);
}

bool console_only_shader(Bytes container);
std::string shader_signature(Bytes container) {
    std::string text = console_only_shader(container) ? "(console code only) " : "";
    for (const auto *name : {"ISG1", "OSG1"}) {
        text += name == std::string("ISG1") ? "in:" : " out:";
        for (const auto &element : signature(container, name))
            text += " " + element.name + std::to_string(element.index) + "/" +
                    std::to_string(element.type) + "x" + std::to_string(element.width) + "@r" +
                    std::to_string(element.reg) + "m" + std::to_string(element.mask);
    }
    return text;
}

std::optional<std::string> console_shader_source(ShaderStage stage, Bytes container,
                                                 std::string *why) {
    return translated_source(stage, container, why);
}

std::string console_shader_probe(std::span<const std::uint32_t> words) {
    // Before the words, what gives them something to work on: a descriptor of the first
    // table in s[12:19] and a sampler of the second in s[24:27].
    static constexpr std::uint32_t before[] = {
        0xbe940305,             // s_mov_b32 s20, s5
        0xf40c0302, 0xfa000000, // s_load_dwordx8 s[12:19], s[4:5], 0
        0xf408060a, 0xfa000000, // s_load_dwordx4 s[24:27], s[20:21], 0
    };
    std::vector<std::uint32_t> code(std::begin(before), std::end(before));
    code.insert(code.end(), words.begin(), words.end());
    // the least of a code file: where the code is and how long, four words of thread group
    // sizes, a hash, the code, and the record that ends the records
    constexpr std::size_t region = 0x140, start = region + 0x28;
    std::vector<std::uint8_t> file(start + 4 * code.size() + 20, 0);
    const auto put = [&](std::size_t at, std::uint32_t value) { std::memcpy(file.data() + at, &value, 4); };
    std::memcpy(file.data(), "FSSM", 4);
    put(0xc0, static_cast<std::uint32_t>(region));
    put(0xc8, static_cast<std::uint32_t>(0x28 + 4 * code.size()));
    for (std::size_t n = 0; n < 3; ++n)
        put(region + 4 + 4 * n, 1);
    for (std::size_t n = 0; n < code.size(); ++n)
        put(start + 4 * n, code[n]);
    put(start + 4 * code.size() + 8, ~0u);
    // A root signature of two tables: resources without end, and samplers. Version 1: the
    // header, two parameters (type, visibility, offset), each table (ranges, offset) and
    // its ranges (type, count, register, space, offset in the table).
    static constexpr std::uint32_t signature[] = {
        1, 2, 24, 0, 48, 0,   // version, parameters and where, static samplers and where, flags
        0, 0, 48, 0, 0, 76,   // the two tables
        1, 56, 0, ~0u, 0, 0, 0, // resources: one range of shader resources from t0
        1, 84, 3, 16, 0, 0, 0,  // samplers: sixteen from s0
    };
    rdna::Shader shader;
    shader.file = file;
    shader.root_signature = {reinterpret_cast<const std::uint8_t *>(signature), sizeof signature};
    std::string why;
    return rdna::translate(ShaderStage::compute, shader, &why) ? std::string{} : why;
}

bool console_only_shader(Bytes container) {
    // The program header says where its bitcode is, from the "DXIL" at offset 8. A shader
    // with console code may have its bitcode as well, and then the host runs that.
    const auto dxil = part(container, "DXIL");
    if (dxil.size() < 28 || std::memcmp(dxil.data() + 24, "XDAT", 4))
        return false;
    const std::size_t bitcode = 8 + std::size_t{word(dxil, 16)};
    static constexpr std::uint8_t magic[] = {0x42, 0x43, 0xc0, 0xde};
    return bitcode + 4 > dxil.size() || std::memcmp(dxil.data() + bitcode, magic, 4) != 0;
}

std::vector<std::uint32_t> console_shader_code(Bytes container) {
    const auto file = code_container(container);
    const Bytes bytes(file);
    // The header has one slot per kind of shader, unused ones filled with a marker; the one
    // in use leads to a record that has where the code is and how long, padding included.
    std::uint32_t record = 0;
    for (std::size_t slot = 0x18; slot < 0x78 && !record; slot += 8)
        if (word(bytes, slot) != 0xdeadbeef)
            record = word(bytes, slot);
    auto start = word(bytes, record + 0x10);
    const auto size = word(bytes, record + 0x18);
    if (!record || start % 4 || start >= bytes.size() || size > bytes.size() - start)
        return {};
    std::vector<std::uint32_t> code;
    constexpr std::uint32_t end_program = 0xbf810000;
    for (auto at = start; at + 4 <= start + size; at += 4) {
        const auto value = word(bytes, at);
        if (code.empty() && !value) // zero words lead in
            continue;
        code.push_back(value);
        if (value == end_program)
            return code;
    }
    return {};
}

namespace {
std::span<const std::uint8_t> make_host_shader(ShaderStage stage, Bytes container) {
    const char letter = "vpc123"[static_cast<int>(stage)];
    const auto hash = hash_name(container);
    // XV_SHADER_STAND_INS: no translation, to tell what a translated shader breaks; with a
    // value, the letters of the stages that are still translated (v, p, c)
    static const char *const stand_ins = std::getenv("XV_SHADER_STAND_INS");
    const char stage_letter = "vpc123"[static_cast<int>(stage)];
    const bool stand_ins_only = stand_ins && !std::strchr(stand_ins, stage_letter);
    const auto name = hash + "-" + letter + "-v" + std::to_string(translator_version) +
                      (stand_ins_only ? "-stand-in" : "") + ".dxbc";
    // one thread makes a shader, and the others that want the same one wait for it: titles
    // make their pipelines on many threads, and the host's compiler is slow
    {
        std::unique_lock lock(mutex);
        made_one.wait(lock, [&] { return !making.count(name); });
        if (const auto found = made.find(name); found != made.end())
            return found->second;
        making.insert(name);
    }
    const auto &folder = shader_cache_directory();
    std::vector<std::uint8_t> shader;
    // XV_SHADER_WHY: every shader made anew, and what stopped the ones with a stand-in
    static const bool asking = std::getenv("XV_SHADER_WHY") != nullptr;
    static std::map<std::string, unsigned> reasons;
    if (!folder.empty() && !asking)
        shader = read_file(folder / name);
    const auto code = console_shader_code(container);
    // XV_SHADER_CODE=<folder>: of every console shader met, the machine code (.bin), the
    // whole of what it was stored in (.fssm) and the shader itself (.dxbc)
    if (const auto *const dump = std::getenv("XV_SHADER_CODE")) {
        const auto base = std::filesystem::path(dump) / (hash + "-" + letter);
        if (!code.empty())
            write_file(base.wstring() + L".bin",
                       {reinterpret_cast<const std::uint8_t *>(code.data()), code.size() * 4});
        write_file(base.wstring() + L".fssm", code_container(container));
        write_file(base.wstring() + L".dxbc", container); // the shader as the title has it
    }
    if (shader.empty()) {
        std::string why = "stand-ins asked for";
        auto source = stand_ins_only ? std::nullopt : translated_source(stage, container, &why);
        // XV_SHADER_SKIP=<text>[,<text>...]: a stand-in for the shaders whose HLSL has any
        // of these in it, to find which kind of shader breaks something
        if (const auto *const skip = std::getenv("XV_SHADER_SKIP"); skip && source) {
            const std::string list = skip;
            for (std::size_t at = 0; at <= list.size() && source;) {
                const auto end = std::min(list.find(',', at), list.size());
                if (end > at && source->find(list.substr(at, end - at)) != std::string::npos)
                    source.reset();
                at = end + 1;
            }
        }
        // XV_SHADER_CODE: the HLSL made for it as well
        if (const auto *const dump = std::getenv("XV_SHADER_CODE"); dump && source) {
            const auto text = std::filesystem::path(dump) / (hash + "-" + letter + ".hlsl");
            write_file(text, {reinterpret_cast<const std::uint8_t *>(source->data()),
                              source->size()});
        }
        const bool kept = !folder.empty() && !asking;
        if (source)
            shader = compile(stage, *source, kept ? folder / name : std::filesystem::path{});
        const bool done = !shader.empty();
        ++(done ? translated : stood_in);
        const std::lock_guard lock(mutex);
        if (!done) {
            if (asking)
                ++reasons[std::string(1, letter) + ": " + (source ? "the host's compiler" : why)];
            const auto text = stand_in(stage, container);
            if (const auto *const dump = std::getenv("XV_SHADER_CODE"))
                write_file(std::filesystem::path(dump) / (hash + "-" + letter + ".stand-in.hlsl"),
                           {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
            shader = compile(stage, text);
        } else if (kept) {
            // only what was translated is kept: a stand-in is made again, in case the next
            // version translates the shader
            write_file(folder / name, shader);
        }
        const auto total = translated + stood_in;
        if (std::has_single_bit(total))
            std::fprintf(stderr,
                         "[Runtime] console shaders made for the host so far: %u translated, %u "
                         "with a stand-in that draws nothing%s\n",
                         translated.load(), stood_in.load(),
                         folder.empty() ? "" : " (kept in the shader cache)");
        if (asking && std::has_single_bit(total) && total >= 256)
            for (const auto &[reason, count] : reasons)
                std::fprintf(stderr, "[Runtime]   %u stopped by %s\n", count, reason.c_str());
    }
    const std::lock_guard lock(mutex);
    making.erase(name);
    made_one.notify_all();
    return made.emplace(name, std::move(shader)).first->second;
}
} // namespace

std::span<const std::uint8_t> host_shader(ShaderStage stage, Bytes container) {
    return make_host_shader(stage, container);
}
} // namespace xv::runtime::modules
