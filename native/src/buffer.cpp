// Pro AudioLab - native C++ audio engine
// buffer.cpp: AudioBuffer, PCM conversion, handle registry.
#include "buffer.h"

#include <cmath>
#include <cstring>

namespace pal {

namespace {
inline int32_t ClampI32(int64_t v, int32_t lo, int32_t hi) {
  return static_cast<int32_t>(v < lo ? lo : (v > hi ? hi : v));
}
inline double ClampF(double v, double lo, double hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
}  // namespace

AudioBuffer::AudioBuffer(size_t frames, uint32_t channels, uint32_t sampleRate)
    : channels_(channels), sampleRate_(sampleRate) {
  if (channels == 0) throw std::invalid_argument("channels must be >= 1");
  if (frames > (((size_t)1 << 40) / channels))
    throw std::invalid_argument("buffer too large");
  data_.assign((size_t)channels * frames, 0.0f);
}

AudioBuffer AudioBuffer::FromPlanar(std::vector<float> planar, uint32_t channels,
                                    uint32_t sampleRate) {
  if (channels == 0) throw std::invalid_argument("channels must be >= 1");
  if (!planar.empty() && planar.size() % channels != 0)
    throw std::invalid_argument("planar data size must be a multiple of channels");
  AudioBuffer b;
  b.channels_ = channels;
  b.sampleRate_ = sampleRate;
  b.data_ = std::move(planar);
  return b;
}

bool ParsePcmFormat(const std::string& name, PcmFormat* out) {
  if (name == "u8") *out = PcmFormat::U8;
  else if (name == "s16" || name == "int16") *out = PcmFormat::S16;
  else if (name == "s24" || name == "int24") *out = PcmFormat::S24;
  else if (name == "s32" || name == "int32") *out = PcmFormat::S32;
  else if (name == "f32" || name == "float32") *out = PcmFormat::F32;
  else if (name == "f64" || name == "float64") *out = PcmFormat::F64;
  else return false;
  return true;
}

int BytesPerPcmFormat(PcmFormat f) {
  switch (f) {
    case PcmFormat::U8: return 1;
    case PcmFormat::S16: return 2;
    case PcmFormat::S24: return 3;
    case PcmFormat::S32: return 4;
    case PcmFormat::F32: return 4;
    case PcmFormat::F64: return 8;
  }
  return 4;
}

std::vector<float> InterleavedToPlanar(const uint8_t* bytes, size_t byteCount,
                                       uint32_t channels, int bytesPer,
                                       bool isFloat, bool unsigned8) {
  if (channels == 0) throw std::invalid_argument("channels must be >= 1");
  if (bytesPer < 1 || bytesPer > 8) throw std::invalid_argument("bad bytesPer");
  if (isFloat && bytesPer != 4 && bytesPer != 8)
    throw std::invalid_argument("float PCM must be 32 or 64 bit");
  if (!isFloat && bytesPer == 8)
    throw std::invalid_argument("integer PCM must be 8/16/24/32 bit");

  const size_t frameBytes = (size_t)channels * (size_t)bytesPer;
  if (frameBytes == 0) return {};
  const size_t frames = byteCount / frameBytes;
  std::vector<float> out((size_t)channels * frames);

  for (size_t i = 0; i < frames; ++i) {
    for (uint32_t c = 0; c < channels; ++c) {
      const uint8_t* p = bytes + (i * (size_t)channels + c) * (size_t)bytesPer;
      float v = 0.0f;
      if (isFloat) {
        if (bytesPer == 4) {
          uint32_t bits = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
          float f;
          std::memcpy(&f, &bits, 4);
          v = f;
        } else {
          uint64_t bits = 0;
          for (int k = 7; k >= 0; --k) bits = (bits << 8) | p[k];
          double d;
          std::memcpy(&d, &bits, 8);
          v = (float)d;
        }
      } else if (bytesPer == 1) {
        int32_t x = unsigned8 ? (int32_t)p[0] - 128 : (int32_t)(int8_t)p[0];
        v = (float)(x / 128.0);
      } else if (bytesPer == 2) {
        int32_t x = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
        if (x & 0x8000) x -= 0x10000;
        v = (float)(x / 32768.0);
      } else if (bytesPer == 3) {
        int32_t x = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                              ((uint32_t)p[2] << 16));
        if (x & 0x800000) x -= 0x1000000;
        v = (float)(x / 8388608.0);
      } else {  // 4
        uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        int32_t x = (int32_t)u;
        v = (float)(x / 2147483648.0);
      }
      out[(size_t)c * frames + i] = v;
    }
  }
  return out;
}

std::vector<uint8_t> PlanarToInterleaved(const AudioBuffer& buf, int bytesPer,
                                         bool isFloat, bool unsigned8) {
  const size_t frames = buf.frames();
  const uint32_t channels = buf.channels();
  std::vector<uint8_t> out(frames * (size_t)channels * (size_t)bytesPer, 0);
  uint8_t* w = out.data();

  for (size_t i = 0; i < frames; ++i) {
    for (uint32_t c = 0; c < channels; ++c) {
      const float s = buf.channel(c)[i];
      const double x = ClampF((double)s, -1.0, 1.0);
      if (isFloat) {
        if (bytesPer == 4) {
          float f = (float)s;
          uint32_t bits;
          std::memcpy(&bits, &f, 4);
          *w++ = (uint8_t)(bits & 0xff);
          *w++ = (uint8_t)((bits >> 8) & 0xff);
          *w++ = (uint8_t)((bits >> 16) & 0xff);
          *w++ = (uint8_t)((bits >> 24) & 0xff);
        } else {
          double d = (double)s;
          uint64_t bits;
          std::memcpy(&bits, &d, 8);
          for (int k = 0; k < 8; ++k) *w++ = (uint8_t)((bits >> (8 * k)) & 0xff);
        }
      } else if (bytesPer == 1) {
        int32_t v = (int32_t)std::lrint(x * (unsigned8 ? 128.0 : 127.0));
        if (unsigned8) v += 128;
        *w++ = (uint8_t)ClampI32(v, unsigned8 ? 0 : -128, unsigned8 ? 255 : 127);
      } else if (bytesPer == 2) {
        int32_t v = ClampI32((int64_t)std::llrint(x * 32768.0), -32768, 32767);
        *w++ = (uint8_t)(v & 0xff);
        *w++ = (uint8_t)((v >> 8) & 0xff);
      } else if (bytesPer == 3) {
        int32_t v = ClampI32((int64_t)std::llrint(x * 8388608.0), -8388608, 8388607);
        *w++ = (uint8_t)(v & 0xff);
        *w++ = (uint8_t)((v >> 8) & 0xff);
        *w++ = (uint8_t)((v >> 16) & 0xff);
      } else {  // 4
        int64_t scaled = (int64_t)std::llrint(x * 2147483648.0);
        int32_t v = ClampI32(scaled, -2147483647LL - 1, 2147483647LL);
        *w++ = (uint8_t)(v & 0xff);
        *w++ = (uint8_t)((v >> 8) & 0xff);
        *w++ = (uint8_t)((v >> 16) & 0xff);
        *w++ = (uint8_t)((v >> 24) & 0xff);
      }
    }
  }
  return out;
}

uint32_t HandleStore::put(std::unique_ptr<AudioBuffer> buffer) {
  std::lock_guard<std::mutex> g(mutex_);
  uint32_t id;
  if (!freeList_.empty()) {
    id = freeList_.back();
    freeList_.pop_back();
    slots_[id - 1] = std::move(buffer);
  } else {
    if (slots_.size() >= 0xFFFFFFFEu) throw std::runtime_error("too many buffers");
    id = (uint32_t)slots_.size() + 1;
    slots_.push_back(std::move(buffer));
  }
  return id;
}

AudioBuffer* HandleStore::get(uint32_t handle) {
  if (handle == 0 || handle > slots_.size()) return nullptr;
  std::lock_guard<std::mutex> g(mutex_);
  return slots_[handle - 1].get();
}

bool HandleStore::free(uint32_t handle) {
  if (handle == 0 || handle > slots_.size()) return false;
  std::lock_guard<std::mutex> g(mutex_);
  if (!slots_[handle - 1]) return false;
  slots_[handle - 1].reset();
  freeList_.push_back(handle);
  return true;
}

size_t HandleStore::liveCount() {
  std::lock_guard<std::mutex> g(mutex_);
  size_t n = 0;
  for (const auto& s : slots_)
    if (s) ++n;
  return n;
}

uint64_t HandleStore::totalFrames() {
  std::lock_guard<std::mutex> g(mutex_);
  uint64_t n = 0;
  for (const auto& s : slots_)
    if (s) n += (uint64_t)s->frames() * s->channels();
  return n;
}

HandleStore& Store() {
  static HandleStore store;
  return store;
}

void ValidateSampleRate(uint32_t rate) {
  if (rate < 1000 || rate > 768000)
    throw std::invalid_argument("sampleRate must be between 1000 and 768000 Hz");
}

void ValidateChannels(uint32_t channels) {
  if (channels < 1 || channels > 32)
    throw std::invalid_argument("channels must be between 1 and 32");
}

}  // namespace pal
