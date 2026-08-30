// Pro AudioLab - native C++ audio engine
// mixer.cpp: multitrack mixdown with resampling, channel conversion, gain/pan.
#include "mixer.h"
#include "dsp.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pal {

std::unique_ptr<AudioBuffer> MixTracks(const std::vector<MixTrack>& tracks,
                                       uint32_t outRate, uint32_t outChannels) {
  std::vector<const AudioBuffer*> live;
  std::vector<double> gains, pans;
  for (const auto& t : tracks) {
    if (t.src && t.src->frames() > 0) {
      live.push_back(t.src);
      gains.push_back(t.gainLin);
      pans.push_back(t.pan);
    }
  }
  if (live.empty())
    throw std::invalid_argument("mix requires at least one non-empty track");

  if (outRate == 0) outRate = live[0]->sampleRate();
  ValidateSampleRate(outRate);
  if (outChannels == 0) {
    for (const AudioBuffer* b : live)
      outChannels = std::max<uint32_t>(outChannels, b->channels());
  }
  ValidateChannels(outChannels);

  constexpr double kPi = 3.14159265358979323846;

  // Stage every track at outRate / outChannels. `owners` keeps converted
  // buffers alive; `ready` points at the staged buffers.
  std::vector<std::unique_ptr<AudioBuffer>> owners;
  std::vector<const AudioBuffer*> ready;
  size_t maxFrames = 0;
  for (const AudioBuffer* b : live) {
    std::unique_ptr<AudioBuffer> tmp;
    if (b->sampleRate() != outRate) {
      tmp = Resample(*b, outRate, /*quality=*/1);
      b = tmp.get();
    }
    if (b->channels() != outChannels) {
      auto conv = ConvertChannels(*b, outChannels);
      tmp = std::move(conv);
      b = tmp.get();
    }
    maxFrames = std::max(maxFrames, b->frames());
    ready.push_back(b);
    owners.push_back(std::move(tmp));
  }

  // Per-track stereo gains (equal-power pan law).
  const size_t nTracks = ready.size();
  std::vector<double> gl(nTracks, 1.0), gr(nTracks, 1.0);
  for (size_t t = 0; t < nTracks; ++t) {
    double pan = std::max(-1.0, std::min(1.0, pans[t]));
    gl[t] = gr[t] = gains[t];
    if (outChannels == 2 && pan != 0.0) {
      if (ready[t]->channels() == 1) {
        const double theta = (pan + 1.0) * kPi / 4.0;
        gl[t] = gains[t] * std::cos(theta);
        gr[t] = gains[t] * std::sin(theta);
      } else if (pan < 0) {  // balance law
        gr[t] = gains[t] * std::cos(-pan * kPi / 2.0);
      } else {
        gl[t] = gains[t] * std::cos(pan * kPi / 2.0);
      }
    }
  }

  // Accumulate in double precision, then clamp once.
  std::vector<double> acc((size_t)maxFrames * outChannels, 0.0);
  for (size_t t = 0; t < nTracks; ++t) {
    const AudioBuffer* b = ready[t];
    const size_t n = b->frames();
    for (size_t i = 0; i < n; ++i) {
      if (outChannels == 2 && b->channels() >= 2) {
        acc[i * 2 + 0] += (double)b->channel(0)[i] * gl[t];
        acc[i * 2 + 1] += (double)b->channel(1)[i] * gr[t];
      } else {
        for (uint32_t c = 0; c < outChannels; ++c) {
          const double g = (outChannels == 2) ? ((c == 0) ? gl[t] : gr[t]) : gains[t];
          acc[i * outChannels + c] += (double)b->channel(c)[i] * g;
        }
      }
    }
  }

  auto out = std::make_unique<AudioBuffer>(maxFrames, outChannels, outRate);
  for (uint32_t c = 0; c < outChannels; ++c) {
    float* y = out->channel(c);
    for (size_t i = 0; i < maxFrames; ++i) {
      const double v = acc[i * outChannels + c];
      y[i] = (float)std::max(-1.0, std::min(1.0, v));
    }
  }
  return out;
}

}  // namespace pal
