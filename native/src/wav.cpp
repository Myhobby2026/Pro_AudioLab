// Pro AudioLab - native C++ audio engine
// wav.cpp: RIFF/RF64 WAV reader & writer. PCM 8/16/24/32-bit and IEEE 32/64-bit
// float, WAVE_FORMAT_EXTENSIBLE, arbitrary chunk ordering, odd-size padding.
#include "wav.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace pal {

namespace {

constexpr double kMaxSamples = 2.0e9;  // safety cap (~8 GB float32 planar)

inline uint32_t RdU32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
inline uint16_t RdU16(const uint8_t* p) {
  return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}
inline void WrU32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}
inline void WrU16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
}

bool ReadAllBytes(const PathStr& path, std::vector<uint8_t>& out) {
#ifdef _WIN32
  FILE* f = _wfopen(path.c_str(), L"rb");
#else
  FILE* f = std::fopen(path.c_str(), "rb");
#endif
  if (!f) return false;
  if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
  const long sz = std::ftell(f);
  if (sz < 0) { std::fclose(f); return false; }
  std::fseek(f, 0, SEEK_SET);
  out.resize((size_t)sz);
  const size_t got = sz > 0 ? std::fread(out.data(), 1, (size_t)sz, f) : 0;
  std::fclose(f);
  return got == (size_t)sz;
}

bool WriteAllBytes(const PathStr& path, const uint8_t* data, size_t len) {
#ifdef _WIN32
  FILE* f = _wfopen(path.c_str(), L"wb");
#else
  FILE* f = std::fopen(path.c_str(), "wb");
#endif
  if (!f) return false;
  const bool ok = (len == 0) || (std::fwrite(data, 1, len, f) == len);
  const bool closed = std::fclose(f) == 0;
  return ok && closed;
}

uint32_t DefaultChannelMask(uint32_t channels) {
  switch (channels) {
    case 1: return 0x00000004;  // FC
    case 2: return 0x00000003;  // FL FR
    case 3: return 0x00000007;  // FL FR FC
    case 4: return 0x00000033;  // quad
    case 5: return 0x00000037;  // 5.0
    case 6: return 0x0000003F;  // 5.1
    case 7: return 0x0000013F;  // 6.1
    case 8: return 0x00000063F; // 7.1
    default: return 0x00000003;
  }
}

}  // namespace

std::unique_ptr<AudioBuffer> LoadWav(const PathStr& path, WavInfo* infoOut) {
  std::vector<uint8_t> raw;
  if (!ReadAllBytes(path, raw))
    throw std::runtime_error("cannot open file (check that the path exists and is readable)");

  if (raw.size() < 12) throw std::runtime_error("not a WAV file (file too small)");
  const bool rf64 = std::memcmp(raw.data(), "RF64", 4) == 0;
  if (!rf64 && std::memcmp(raw.data(), "RIFF", 4) != 0)
    throw std::runtime_error("not a RIFF/WAV file");
  if (std::memcmp(raw.data() + 8, "WAVE", 4) != 0)
    throw std::runtime_error("not a WAV file (missing WAVE tag)");

  bool haveFmt = false;
  uint16_t fmtTag = 0, bits = 0, extFmt = 0;
  uint32_t channels = 0, rate = 0;
  size_t dataOff = 0;
  uint64_t dataSize = 0;
  uint64_t rf64DataSize = 0;
  bool sawData = false;

  size_t pos = 12;
  while (pos + 8 <= raw.size()) {
    char id[5] = {0, 0, 0, 0, 0};
    std::memcpy(id, raw.data() + pos, 4);
    const uint32_t sz = RdU32(raw.data() + pos + 4);
    const size_t body = pos + 8;
    uint64_t lim = (uint64_t)body + sz;
    if (lim > raw.size()) lim = raw.size();  // tolerate truncated files

    if (std::strcmp(id, "ds64") == 0 && rf64) {
      if (lim >= body + 16) {
        rf64DataSize = (uint64_t)RdU32(raw.data() + body + 8) |
                       ((uint64_t)RdU32(raw.data() + body + 12) << 32);
      }
    } else if (std::strcmp(id, "fmt ") == 0) {
      if (lim < body + 16) throw std::runtime_error("corrupt fmt chunk");
      fmtTag = RdU16(raw.data() + body + 0);
      channels = RdU16(raw.data() + body + 2);
      rate = RdU32(raw.data() + body + 4);
      bits = RdU16(raw.data() + body + 14);
      if (fmtTag == 0xFFFE) {  // WAVE_FORMAT_EXTENSIBLE
        if (lim < body + 40) throw std::runtime_error("extensible fmt chunk too small");
        extFmt = RdU16(raw.data() + body + 24);  // first 2 bytes of SubFormat GUID
      }
      haveFmt = true;
    } else if (std::strcmp(id, "data") == 0) {
      dataOff = body;
      dataSize = sz;
      sawData = true;
    }

    const uint64_t next = (uint64_t)pos + 8ull + sz + (sz & 1u);
    if (next <= pos) break;
    pos = (size_t)next;
  }

  if (!haveFmt) throw std::runtime_error("WAV file has no fmt chunk");
  if (!sawData) throw std::runtime_error("WAV file has no data chunk");

  const uint16_t realTag = (fmtTag == 0xFFFE) ? extFmt : fmtTag;
  const bool isFloat = (realTag == 3);
  if (realTag != 1 && realTag != 3)
    throw std::runtime_error("unsupported WAV encoding (only uncompressed PCM / IEEE float; "
                             "convert MP3/AAC-compressed files to WAV first)");
  if (isFloat && bits != 32 && bits != 64)
    throw std::runtime_error("float WAV must be 32 or 64 bit");
  if (!isFloat && bits != 8 && bits != 16 && bits != 24 && bits != 32)
    throw std::runtime_error("unsupported PCM bit depth (8/16/24/32 only)");

  ValidateChannels(channels);
  ValidateSampleRate(rate);

  if (rf64 && dataSize == 0xFFFFFFFF) dataSize = rf64DataSize;
  const uint64_t available = raw.size() - dataOff;
  if (dataSize > available) dataSize = available;

  const int bytesPer = bits / 8;
  const uint64_t frameBytes = (uint64_t)channels * (uint64_t)bytesPer;
  if (frameBytes == 0) throw std::runtime_error("corrupt WAV header");
  uint64_t frames = dataSize / frameBytes;
  if (frames * channels > kMaxSamples)
    throw std::runtime_error("file too large (>2 giga-samples)");

  std::vector<float> planar = InterleavedToPlanar(
      raw.data() + dataOff, (size_t)(frameBytes * frames), channels, bytesPer,
      isFloat, /*unsigned8=*/!isFloat && bits == 8);

  auto buf = std::make_unique<AudioBuffer>(
      AudioBuffer::FromPlanar(std::move(planar), channels, rate));

  if (infoOut) {
    infoOut->channels = channels;
    infoOut->sampleRate = rate;
    infoOut->bits = bits;
    infoOut->isFloat = isFloat;
    infoOut->frames = buf->frames();
    infoOut->formatTag = isFloat ? "float" : "pcm";
  }
  return buf;
}

size_t WriteWav(const PathStr& path, const AudioBuffer& buf, uint16_t bits,
                bool isFloat) {
  if (isFloat && bits != 32 && bits != 64)
    throw std::invalid_argument("float WAV requires bits=32 or 64");
  if (!isFloat && bits != 8 && bits != 16 && bits != 24 && bits != 32)
    throw std::invalid_argument("integer WAV requires bits=8/16/24/32");

  const uint32_t channels = buf.channels();
  const uint32_t rate = buf.sampleRate();
  const int bytesPer = bits / 8;

  const std::vector<uint8_t> pcm = PlanarToInterleaved(
      buf, bytesPer, isFloat, /*unsigned8=*/!isFloat && bits == 8);
  const uint64_t dataBytes = (uint64_t)pcm.size();
  const uint32_t pad = (uint32_t)(dataBytes & 1);  // RIFF chunks are word aligned

  const bool extensible = channels > 2;
  const uint16_t fmtTag = extensible ? (uint16_t)0xFFFE
                                     : (uint16_t)(isFloat ? 3 : 1);
  const uint16_t fmtSize = extensible ? 40 : 16;
  const uint64_t fmtChunkBytes = 8ull + fmtSize;
  const uint64_t dataChunkBytes = 8ull + dataBytes + pad;
  const uint64_t riffSize = 4ull + fmtChunkBytes + dataChunkBytes;
  if (riffSize > 0xFFFFFFFFull && !extensible)
    throw std::runtime_error("result exceeds 4 GB WAV limit (use float64 or split)");

  std::vector<uint8_t> out;
  out.reserve((size_t)(8 + riffSize));
  auto w4 = [&](const char* s) { for (int i = 0; i < 4; ++i) out.push_back((uint8_t)s[i]); };

  w4("RIFF");
  uint8_t u32[4];
  WrU32(u32, (uint32_t)riffSize);
  for (int i = 0; i < 4; ++i) out.push_back(u32[i]);
  w4("WAVE");

  w4("fmt ");
  WrU32(u32, fmtSize);
  for (int i = 0; i < 4; ++i) out.push_back(u32[i]);
  uint8_t u16[2];
  WrU16(u16, fmtTag);           out.push_back(u16[0]); out.push_back(u16[1]);
  WrU16(u16, (uint16_t)channels); out.push_back(u16[0]); out.push_back(u16[1]);
  WrU32(u32, rate);             for (int i = 0; i < 4; ++i) out.push_back(u32[i]);
  WrU32(u32, (uint32_t)(rate * channels * bytesPer));  // byte rate
  for (int i = 0; i < 4; ++i) out.push_back(u32[i]);
  WrU16(u16, (uint16_t)(channels * bytesPer));  // block align
  out.push_back(u16[0]); out.push_back(u16[1]);
  WrU16(u16, bits);              out.push_back(u16[0]); out.push_back(u16[1]);
  if (extensible) {
    WrU16(u16, 22);              out.push_back(u16[0]); out.push_back(u16[1]);  // cbSize
    WrU16(u16, bits);            out.push_back(u16[0]); out.push_back(u16[1]);  // valid bits
    WrU32(u32, DefaultChannelMask(channels));
    for (int i = 0; i < 4; ++i) out.push_back(u32[i]);
    // SubFormat GUID: 00000001-0000-0010-8000-00AA00389B71 (PCM)
    //                  00000003-...                    (IEEE float)
    const uint8_t guid[16] = {(uint8_t)(isFloat ? 3 : 1), 0, 0, 0, 0, 0, 0x10, 0,
                              0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
    for (uint8_t b : guid) out.push_back(b);
  }

  w4("data");
  WrU32(u32, (uint32_t)std::min<uint64_t>(dataBytes, 0xFFFFFFFFull));
  for (int i = 0; i < 4; ++i) out.push_back(u32[i]);
  out.insert(out.end(), pcm.begin(), pcm.end());
  if (pad) out.push_back(0);

  if (!WriteAllBytes(path, out.data(), out.size()))
    throw std::runtime_error("failed to write file (check the path and permissions)");
  return out.size();
}

}  // namespace pal
