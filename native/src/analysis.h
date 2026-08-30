// Pro AudioLab - native C++ audio engine
// analysis.h: level metering and waveform peak computation.
#pragma once

#include "buffer.h"

#include <vector>

namespace pal {

struct LevelStats {
  double peakLin = 0.0;
  double peakDb = -999.0;  // -999 == digital silence
  double rmsLin = 0.0;
  double rmsDb = -999.0;
  double dcOffset = 0.0;
};

LevelStats AnalyzeChannel(const float* x, size_t n);

// Per-bucket [min, max] pairs, flattened: [min0, max0, min1, max1, ...]
std::vector<float> ComputePeaks(const float* x, size_t n, size_t buckets);

}  // namespace pal
