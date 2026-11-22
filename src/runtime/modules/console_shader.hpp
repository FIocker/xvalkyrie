#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Shaders a console build ships without the bitcode the host runs: their DXIL part holds
// machine code for the console's GPU instead (see docs/research/console-shaders.md). A host
// shader is made for each one and kept in the shader cache, by the hash of the original.
namespace xv::runtime::modules {
// geometry_points, _lines and _triangles: a geometry shader, by what it is given
enum class ShaderStage {
    vertex,
    pixel,
    compute,
    geometry_points,
    geometry_lines,
    geometry_triangles
};
// whether the container's DXIL part has no bitcode
bool console_only_shader(std::span<const std::uint8_t> container);
// The machine code of such a shader, as little-endian words up to and including the one
// that ends the program. Empty if it is not found.
std::vector<std::uint32_t> console_shader_code(std::span<const std::uint8_t> container);
// The HLSL made for a console shader, or nothing and in `why` what stopped it. For tools
// and tests: `host_shader` is what a title's pipelines get.
std::optional<std::string> console_shader_source(ShaderStage stage,
                                                 std::span<const std::uint8_t> container,
                                                 std::string *why = nullptr);
// What stops the translation of a compute shader made of these words of machine code and
// nothing else, or nothing if it is translated. For tools: which instructions are handled.
// With `compiled`, what is translated is given to the host's compiler too, and what that
// says against it is the answer.
std::string console_shader_probe(std::span<const std::uint32_t> words, bool compiled = false);
// the inputs and outputs of a shader, by semantic, for a line of a log
std::string shader_signature(std::span<const std::uint8_t> container);
// The host's shader for it, from the cache or made now. It stays where it is for as long
// as the process runs. Empty if none could be made.
std::span<const std::uint8_t> host_shader(ShaderStage stage,
                                          std::span<const std::uint8_t> container);
} // namespace xv::runtime::modules
