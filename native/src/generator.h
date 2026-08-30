// Pro AudioLab - native C++ audio engine
// generator.h: test-tone / waveform generators.
#pragma once

#include "buffer.h"

#include <memory>
#include <string>

namespace pal {

// wave: "sine" | "square" | "saw" | "triangle" | "noise" | "silence" | "impulse"
// All channels carry the same signal (noise uses an independent stream).
// Throws std::invalid_argument on bad parameters.
std::unique_ptr<AudioBuffer> GenerateTone(const std::string& wave, double freqHz,
                                          double durationSec, uint32_t sampleRate,
                                          uint32_t channels, double amplitude);

}  // namespace pal
