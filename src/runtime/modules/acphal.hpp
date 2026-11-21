#pragma once

namespace xv::runtime {
class Registry;
namespace modules {
// AcpHal.dll (the audio processor, without SHAPE) and xapu.dll (the Series audio decoder)
void install_acphal(Registry &registry);
// the first development kits have another form of the processor's command messages
void early_audio_processor(bool yes);
} // namespace modules
} // namespace xv::runtime
