#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Shaders a console build ships without the bitcode the host runs: their DXIL part holds
// machine code for the console's GPU instead (see docs/research/console-shaders.md). A host
// shader is made for each one and kept in the shader cache, by the hash of the original.
namespace xs::runtime::modules {
enum class ShaderStage { vertex, pixel, compute };
// whether the container's DXIL part has no bitcode
bool console_only_shader(std::span<const std::uint8_t> container);
// The machine code of such a shader, as little-endian words up to and including the one
// that ends the program. Empty if it is not found.
std::vector<std::uint32_t> console_shader_code(std::span<const std::uint8_t> container);
// The host's shader for it, from the cache or made now. It stays where it is for as long
// as the process runs. Empty if none could be made.
std::span<const std::uint8_t> host_shader(ShaderStage stage,
                                          std::span<const std::uint8_t> container);
} // namespace xs::runtime::modules
