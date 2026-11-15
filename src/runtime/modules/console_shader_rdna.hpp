#pragma once
#include "runtime/modules/console_shader.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xs::runtime::modules::rdna {
using Stage = ShaderStage;
// One element of a shader's input or output signature. They are kept in the order of their
// registers, and element n is the field `fn` of the HLSL struct made for the signature.
struct Element {
    std::string name;
    std::uint32_t index = 0;
    std::uint32_t type = 3;  // 1 uint, 2 int, 3 float
    std::uint32_t width = 4; // components
    bool system_value = false;
};
struct Shader {
    std::vector<std::uint32_t> code;               // with the six words that lead it in
    std::span<const std::uint8_t> root_signature;  // the RTS0 part
    std::vector<Element> inputs, outputs;
};
// The HLSL for the code: what the shader declares and its `main`, which takes `Input input`
// and returns `Output`. Nothing if the code has something that is not handled.
std::optional<std::string> translate(Stage stage, const Shader &shader);
} // namespace xs::runtime::modules::rdna
