#pragma once

#include "sidescopes/module.h"

namespace sidescopes {
[[nodiscard]] const SsScopeDescriptor* hdrWaveformDescriptor();
[[nodiscard]] SsScopeInstance* createHdrWaveform();
}  // namespace sidescopes
