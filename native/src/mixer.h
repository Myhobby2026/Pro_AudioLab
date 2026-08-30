// Pro AudioLab - native C++ audio engine
// mixer.h: multitrack mixdown.
#pragma once

#include "buffer.h"

#include <memory>
#include <vector>

namespace pal {

struct MixTrack {
  const AudioBuffer* src = nullptr;
  double gainLin = 1.0;
  double pan = 0.0;  // -1..1
};

// Mixes tracks into one buffer.
// outRate == 0 -> first track's rate; outChannels == 0 -> widest track
// (any stereo track yields stereo). Tracks are resampled/converted as needed.
std::unique_ptr<AudioBuffer> MixTracks(const std::vector<MixTrack>& tracks,
                                       uint32_t outRate, uint32_t outChannels);

}  // namespace pal
