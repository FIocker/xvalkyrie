#pragma once
#include "runtime/modules/console_shader.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xv::runtime::modules::rdna {
using Stage = ShaderStage;
// One element of a shader's input or output signature. They are kept in the order of their
// registers, and element n is the field `fn` of the HLSL struct made for the signature.
struct Element {
    std::string name;
    std::uint32_t index = 0;
    std::uint32_t type = 3;  // 1 uint, 2 int, 3 float
    std::uint32_t width = 4; // components
    bool system_value = false;
    std::uint32_t reg = 0; // the register it is in, and which of its components
    std::uint8_t mask = 0;
    std::uint8_t read = 0; // the components the shader reads (an input)
    // an input of a program that fetches its vertices itself: the vertex stream it is in
    // and where in a vertex of that stream
    std::uint32_t stream = 0, offset = 0, format = 0;
};
struct Shader {
    std::span<const std::uint8_t> file;           // the FSSM file: code and what goes with it
    std::span<const std::uint8_t> root_signature; // the RTS0 part
    std::vector<Element> inputs, outputs;
    std::span<const std::uint8_t> bindings; // of an Xbox One shader: what it binds, by kind
    // the inputs, by their place in `inputs`, that come as unsigned 10-bit fields of what
    // the title has as signed ones
    std::uint32_t signed_inputs = 0;
    // A program of a cached pipeline is entered with the address of its vertex streams'
    // buffer descriptors in s0:s1 and reads each input from them, by the vertex's number.
    bool streams = false;
    // Such a program knows its descriptors only by where they are in a table, whatever the
    // ranges of the root signature the title draws with say: the translation reads each
    // from the host's heap itself. Its user registers, from s8 on, have the root
    // signature's parameters in order (a table one, constants one each, a root view two),
    // then the address of each heap the signature lets shaders index; the translation has
    // them as `xv_ud<n>` (root constants of the host's, n from 0 for s8): a table's is the
    // number of its first descriptor in the heap, constants are themselves, the heaps'
    // addresses are 0, so that what the program adds up to an address is the byte offset in
    // the heap. `bound`: what the program's records say is at each place of a table (the
    // type as a root signature's range has it), by the register the table is in.
    struct Bound {
        std::uint32_t offset = 0, type = 0; // in bytes from the table's start
        std::uint32_t reg = 8;
    };
    bool heaps = false;
    std::vector<Bound> bound;
};
// The HLSL for the code: what the shader declares and its `main`. A vertex or pixel shader's
// takes `Input input` and returns `Output`; a compute shader's takes what it needs. Nothing
// if the code has something that is not handled, and then `why` says what and `at` the
// instruction it was met at, in words from the first, or ~0. `needs_sm6` reports whether
// the generated shader uses Shader Model 6 intrinsics.
std::optional<std::string> translate(Stage stage, const Shader &shader, std::string *why = nullptr,
                                     std::uint32_t *at = nullptr, bool *needs_sm6 = nullptr);
} // namespace xv::runtime::modules::rdna
