#pragma once

// Sound output of the host. So far only what `exec --mute` needs.
namespace xv::audio {
// Mutes everything this process plays, before or after it starts playing. False if the
// host has no way to do that.
bool mute_process();
} // namespace xv::audio
