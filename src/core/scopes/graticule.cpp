#include "core/scopes/graticule.h"

#include <string>

// The waveform scale is engine-agnostic, so it stays in core and every module
// links it without pulling in a scope engine. The vectorscope graticule, which
// projects colors through the vectorscope engine, lives in
// vectorscope_graticule.cpp alongside that engine instead.

namespace sidescopes {

std::vector<WaveformScaleLine> buildWaveformScale()
{
    std::vector<WaveformScaleLine> lines;
    for (int step = 0; step <= 10; ++step) {
        WaveformScaleLine line;
        line.levelPercent = static_cast<float>(100 - step * 10);
        line.y = static_cast<float>(step) / 10.0f;
        line.major = step % 5 == 0;
        line.label = std::to_string(100 - step * 10);
        lines.push_back(std::move(line));
    }
    return lines;
}

}  // namespace sidescopes
