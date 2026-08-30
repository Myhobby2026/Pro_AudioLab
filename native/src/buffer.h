// Pro AudioLab - native C++ audio engine
// buffer.h: planar multichannel sample buffer, PCM conversion, buffer handle registry.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace pal {

// Planar multichannel float buffer.
// Channel c, frame i lives at data[c * frames + i].
class AudioBuffer {
 public:
  AudioBuffer() = default;
  AudioBuffer(size_t frames, uint32_t channels, uint32_t sampleRate);

  uint32_t channels() const { return channels_; }
  uint32_t sampleRate() const { return sampleRate_; }
  size_t frames() const { return channels_ == 0 ? 0 : data_.size() / channels_; }
  double durationSec() const {
    return sampleRate_ == 0 ? 0.0 : static_cast<double>(frames()) / sampleRate_;
  }

  // Pointer to the first sample of channel c.
  float* channel(size_t c) { return data_.data() + c * frames(); }
  const float* channel(size_t c) const { return data_.data() + c * frames(); }

  void setSampleRate(uint32_t sr) { sampleRate_ = sr; }

  // Takes ownership of planar data (size must be a multiple of channels).
  static AudioBuffer FromPlanar(std::vector<float> planar, uint32_t channels,
                                uint32_t sampleRate);

 private:
  uint32_t channels_ = 0;
  uint32_t sampleRate_ = 0;
  std::vector<float> data_;
};

// PCM sample formats used on the JS bridge (WAV files additionally support
// packed 24-bit, handled internally).
enum class PcmFormat { U8, S16, S24, S32, F32, F64 };

// Returns false when the name is unknown.
bool ParsePcmFormat(const std::string& name, PcmFormat* out);
int BytesPerPcmFormat(PcmFormat f);

// Interleaved PCM bytes -> planar float samples.
// bytesPer: 1 (u8), 2 (s16), 3 (packed s24), 4 (s32 or f32), 8 (f64).
// unsigned8: 1-byte data is unsigned (WAV style).
// Throws std::invalid_argument on bad arguments.
std::vector<float> InterleavedToPlanar(const uint8_t* bytes, size_t byteCount,
                                       uint32_t channels, int bytesPer,
                                       bool isFloat, bool unsigned8);

// Planar buffer -> interleaved PCM bytes (same bytesPer convention).
std::vector<uint8_t> PlanarToInterleaved(const AudioBuffer& buf, int bytesPer,
                                         bool isFloat, bool unsigned8);

// Registry that hands out stable integer handles to live AudioBuffers.
class HandleStore {
 public:
  // Returns handle id >= 1.
  uint32_t put(std::unique_ptr<AudioBuffer> buffer);
  // Returns nullptr when the handle is unknown / already freed.
  AudioBuffer* get(uint32_t handle);
  bool free(uint32_t handle);

  size_t liveCount();
  uint64_t totalFrames();

 private:
  std::mutex mutex_;
  std::vector<std::unique_ptr<AudioBuffer>> slots_;
  std::vector<uint32_t> freeList_;
};

// Process-wide store (each Electron renderer has its own addon instance).
HandleStore& Store();

// Throws std::invalid_argument with a consistent message.
void ValidateSampleRate(uint32_t rate);
void ValidateChannels(uint32_t channels);

}  // namespace pal
