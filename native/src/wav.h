// Pro AudioLab - native C++ audio engine
// wav.h: WAV (RIFF/RF64) reading and writing.
#pragma once

#include "buffer.h"

#include <memory>
#include <string>

namespace pal {

#ifdef _WIN32
using PathStr = std::wstring;  // wide, Unicode-safe on Windows
#else
using PathStr = std::string;
#endif

struct WavInfo {
  uint32_t channels = 0;
  uint32_t sampleRate = 0;
  uint16_t bits = 0;
  bool isFloat = false;
  uint64_t frames = 0;
  std::string formatTag;  // "pcm" or "float"
};

// Loads a WAV file into a planar float buffer.
// Throws std::runtime_error / std::invalid_argument on failure.
std::unique_ptr<AudioBuffer> LoadWav(const PathStr& path, WavInfo* infoOut);

// Writes a buffer to a WAV file.
// bits: 8/16/24/32 integer, or 32/64 with isFloat=true.
// Returns the number of bytes written.
size_t WriteWav(const PathStr& path, const AudioBuffer& buf, uint16_t bits,
                bool isFloat);

}  // namespace pal
