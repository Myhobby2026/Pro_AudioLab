// Pro AudioLab - native C++ audio engine
// generator.cpp: sine/square/saw/triangle/noise/silence/impulse generators.
#include "generator.h"

#include <cmath>
#include <random>
#include <stdexcept>

namespace pal {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

std::unique_ptr<AudioBuffer> GenerateTone(const std::string& wave, double freqHz,
                                          double durationSec, uint32_t sampleRate,
                                          uint32_t channels, double amplitude) {
  ValidateSampleRate(sampleRate);
  ValidateChannels(channels);
  if (durationSec < 0.0 || durationSec > 3600.0)
    throw std::invalid_argument("durationSec must be between 0 and 3600");
  if (amplitude < 0.0 || amplitude > 1.0)
    throw std::invalid_argument("amplitude must be between 0 and 1");
  if (wave != "noise" && wave != "silence" && wave != "impulse") {
    if (freqHz < 0.1 || freqHz > sampleRate * 0.5)
      throw std::invalid_argument("freqHz must be between 0.1 and Nyquist");
  }

  const size_t frames = (size_t)std::llround(durationSec * sampleRate);
  auto out = std::make_unique<AudioBuffer>(frames, channels, sampleRate);

  if (wave == "silence") return out;

  if (wave == "impulse") {
    if (frames > 0)
      for (uint32_t c = 0; c < channels; ++c) out->channel(c)[0] = (float)amplitude;
    return out;
  }

  if (wave == "noise") {
    std::mt19937_64 rng(0x50726F41);  // deterministic seed ("ProA")
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    for (uint32_t c = 0; c < channels; ++c) {
      float* x = out->channel(c);
      for (size_t i = 0; i < frames; ++i) x[i] = (float)(dist(rng) * amplitude);
    }
    return out;
  }

  const double w = kPi * 2.0 * freqHz / (double)sampleRate;
  for (uint32_t c = 0; c < channels; ++c) {
    float* x = out->channel(c);
    for (size_t i = 0; i < frames; ++i) {
      const double t = (double)i;
      double v = 0.0;
      if (wave == "sine") {
        v = std::sin(w * t);
      } else if (wave == "square") {
        v = std::fmod(w * t, kPi * 2.0) < kPi ? 1.0 : -1.0;
      } else if (wave == "saw") {
        const double ph = std::fmod(w * t / (kPi * 2.0), 1.0);
        v = 2.0 * ph - 1.0;
      } else if (wave == "triangle") {
        const double ph = std::fmod(w * t / (kPi * 2.0) + 0.25, 1.0);
        v = 1.0 - 4.0 * std::fabs(ph - 0.5);
      } else {
        throw std::invalid_argument(
            "unknown wave (use sine|square|saw|triangle|noise|silence|impulse)");
      }
      x[i] = (float)(v * amplitude);
    }
  }
  return out;
}

}  // namespace pal
