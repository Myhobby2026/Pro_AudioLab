// Pro AudioLab - native C++ audio engine
// napi_bindings.cpp: N-API glue exposing the engine to JavaScript/Electron.
//
// Conventions:
//  * Buffers live engine-side and are referenced by integer handles (>= 1).
//  * All processing functions return a NEW handle (immutable style), so the
//    UI can keep originals for undo.
//  * All errors are thrown as JavaScript Error objects.
#include <napi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "analysis.h"
#include "buffer.h"
#include "dsp.h"
#include "generator.h"
#include "mixer.h"
#include "wav.h"

namespace {

constexpr const char* kVersion = "ProAudioLab native audio engine 1.0.0";

// ------------------------------------------------------------- helpers
pal::AudioBuffer* BufArg(const Napi::CallbackInfo& info, size_t idx) {
  if (info.Length() <= idx || !info[idx].IsNumber()) return nullptr;
  return pal::Store().get(info[idx].As<Napi::Number>().Uint32Value());
}

double NumArg(const Napi::CallbackInfo& info, size_t idx, double def) {
  if (info.Length() <= idx || !info[idx].IsNumber()) return def;
  return info[idx].As<Napi::Number>().DoubleValue();
}

bool HasOpts(const Napi::CallbackInfo& info, size_t idx) {
  return info.Length() > idx && info[idx].IsObject() && !info[idx].IsArray() &&
         !info[idx].IsFunction() && !info[idx].IsTypedArray();
}

double OptNum(const Napi::CallbackInfo& info, size_t idx, const char* key,
              double def) {
  if (!HasOpts(info, idx)) return def;
  Napi::Object o = info[idx].As<Napi::Object>();
  Napi::Value v = o.Get(key);
  if (v.IsUndefined() || v.IsNull()) return def;
  if (!v.IsNumber())
    throw std::invalid_argument(std::string("option '") + key + "' must be a number");
  return v.As<Napi::Number>().DoubleValue();
}

std::string OptStr(const Napi::CallbackInfo& info, size_t idx, const char* key,
                   const std::string& def) {
  if (!HasOpts(info, idx)) return def;
  Napi::Object o = info[idx].As<Napi::Object>();
  Napi::Value v = o.Get(key);
  if (v.IsUndefined() || v.IsNull()) return def;
  if (!v.IsString())
    throw std::invalid_argument(std::string("option '") + key + "' must be a string");
  return v.As<Napi::String>().Utf8Value();
}

// Reads a string either positionally (info[idx] is a string) or from an
// options object at that position: f(h, "...", ...) or f(h, {key: "..."}).
std::string StrArgOrOpt(const Napi::CallbackInfo& info, size_t idx,
                        const char* key, const std::string& def) {
  if (info.Length() > idx && info[idx].IsString())
    return info[idx].As<Napi::String>().Utf8Value();
  return OptStr(info, idx, key, def);
}

// Accepts a boolean or number option.
double OptBoolOrNum(const Napi::CallbackInfo& info, size_t idx, const char* key,
                    double def) {
  if (!HasOpts(info, idx)) return def;
  Napi::Object o = info[idx].As<Napi::Object>();
  Napi::Value v = o.Get(key);
  if (v.IsUndefined() || v.IsNull()) return def;
  if (v.IsBoolean()) return v.As<Napi::Boolean>().Value() ? 1 : 0;
  if (v.IsNumber()) return v.As<Napi::Number>().DoubleValue();
  throw std::invalid_argument(std::string("option '") + key + "' must be boolean or number");
}

double ClampOpt(double v, double lo, double hi, const char* name) {
  if (v < lo || v > hi)
    throw std::invalid_argument(std::string("option '") + name + "' out of range");
  return v;
}

Napi::Number NewHandle(Napi::Env env, std::unique_ptr<pal::AudioBuffer> buf) {
  return Napi::Number::New(env, (double)pal::Store().put(std::move(buf)));
}

Napi::Object BufferInfoJs(Napi::Env env, const pal::AudioBuffer& b) {
  Napi::Object o = Napi::Object::New(env);
  o.Set("channels", (double)b.channels());
  o.Set("sampleRate", (double)b.sampleRate());
  o.Set("frames", (double)b.frames());
  o.Set("durationSec", b.durationSec());
  return o;
}

// Reads argument `idx` as a file path (UTF-16 on Windows, UTF-8 elsewhere).
pal::PathStr PathArg(const Napi::CallbackInfo& info, size_t idx) {
  if (info.Length() <= idx || !info[idx].IsString())
    throw std::invalid_argument("expected a string file path");
#ifdef _WIN32
  std::u16string u16 = info[idx].As<Napi::String>().Utf16Value();
  return std::wstring(u16.begin(), u16.end());
#else
  return info[idx].As<Napi::String>().Utf8Value();
#endif
}

// Reads argument `idx` as raw PCM bytes (ArrayBuffer or TypedArray view).
void BytesArg(const Napi::CallbackInfo& info, size_t idx, const uint8_t** data,
              size_t* len) {
  if (info.Length() <= idx) throw std::invalid_argument("missing PCM data argument");
  const Napi::Value& v = info[idx];
  if (v.IsTypedArray()) {
    Napi::TypedArray ta = v.As<Napi::TypedArray>();
    Napi::ArrayBuffer ab = ta.ArrayBuffer();
    *data = reinterpret_cast<const uint8_t*>(ab.Data()) + ta.ByteOffset();
    *len = ta.ByteLength();
    return;
  }
  if (v.IsArrayBuffer()) {
    Napi::ArrayBuffer ab = v.As<Napi::ArrayBuffer>();
    *data = reinterpret_cast<const uint8_t*>(ab.Data());
    *len = ab.ByteLength();
    return;
  }
  throw std::invalid_argument("PCM data must be an ArrayBuffer or TypedArray");
}

Napi::Value ThrowCpp(Napi::Env env, const std::exception& e) {
  Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
  return env.Null();
}

Napi::Object StatsToJs(Napi::Env env, const pal::LevelStats& s) {
  Napi::Object o = Napi::Object::New(env);
  o.Set("peak", s.peakLin);
  o.Set("peakDb", s.peakDb <= -998.0 ? -INFINITY : s.peakDb);
  o.Set("rms", s.rmsLin);
  o.Set("rmsDb", s.rmsDb <= -998.0 ? -INFINITY : s.rmsDb);
  o.Set("dcOffset", s.dcOffset);
  return o;
}

// ------------------------------------------------------------- bindings
Napi::Value Version(const Napi::CallbackInfo& info) {
  return Napi::String::New(info.Env(), kVersion);
}

Napi::Value CreateBuffer(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const double frames = NumArg(info, 0, -1);
    const double channels = NumArg(info, 1, 0);
    const double rate = NumArg(info, 2, 0);
    if (frames < 0 || frames > 2.0e9)
      throw std::invalid_argument("frames must be between 0 and 2e9");
    pal::ValidateChannels((uint32_t)channels);
    pal::ValidateSampleRate((uint32_t)rate);
    return NewHandle(env, std::make_unique<pal::AudioBuffer>(
                              (size_t)frames, (uint32_t)channels, (uint32_t)rate));
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value FreeBuffer(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    if (info.Length() < 1 || !info[0].IsNumber())
      throw std::invalid_argument("expected a buffer handle");
    return Napi::Boolean::New(
        env, pal::Store().free(info[0].As<Napi::Number>().Uint32Value()));
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value Info(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    return BufferInfoJs(env, *b);
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value Stats(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Object o = Napi::Object::New(env);
  o.Set("liveBuffers", (double)pal::Store().liveCount());
  o.Set("totalSamples", (double)pal::Store().totalFrames());
  return o;
}

Napi::Value LoadWav(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::PathStr path = PathArg(info, 0);
    pal::WavInfo wi;
    std::unique_ptr<pal::AudioBuffer> buf = pal::LoadWav(path, &wi);
    const uint32_t handle = pal::Store().put(std::move(buf));
    Napi::Object o = BufferInfoJs(env, *pal::Store().get(handle));
    o.Set("handle", (double)handle);
    o.Set("bits", (double)wi.bits);
    o.Set("isFloat", wi.isFloat);
    o.Set("format", wi.formatTag);
    return o;
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value WriteWav(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    const pal::PathStr path = PathArg(info, 1);
    double bits = 16;
    if (info.Length() > 2 && info[2].IsNumber()) bits = info[2].As<Napi::Number>().DoubleValue();
    else bits = OptNum(info, 2, "bits", 16);
    const bool isFloat = OptBoolOrNum(info, 2, "float", 0) != 0 ||
                         OptBoolOrNum(info, 3, "float", 0) != 0;
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32 && bits != 64)
      throw std::invalid_argument("bits must be 8, 16, 24, 32 or 64");
    const size_t bytes = pal::WriteWav(path, *b, (uint16_t)bits, isFloat);
    return Napi::Number::New(env, (double)bytes);
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value GenerateTone(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const std::string wave = OptStr(info, 0, "wave", "sine");
    const double freq = OptNum(info, 0, "freqHz", 440);
    const double dur = OptNum(info, 0, "durationSec", 1);
    const double rate = OptNum(info, 0, "sampleRate", 44100);
    const double ch = OptNum(info, 0, "channels", 1);
    const double amp = OptNum(info, 0, "amplitude", 0.5);
    return NewHandle(env, pal::GenerateTone(wave, freq, dur, (uint32_t)rate,
                                            (uint32_t)ch, amp));
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value ToPCM(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    const std::string fmtName = StrArgOrOpt(info, 1, "format", "f32");
    pal::PcmFormat fmt;
    if (!pal::ParsePcmFormat(fmtName, &fmt))
      throw std::invalid_argument("format must be u8|s16|s24|s32|f32|f64");
    std::vector<uint8_t> pcm =
        pal::PlanarToInterleaved(*b, pal::BytesPerPcmFormat(fmt),
                                 fmt == pal::PcmFormat::F32 || fmt == pal::PcmFormat::F64,
                                 fmt == pal::PcmFormat::U8);
    Napi::ArrayBuffer ab = Napi::ArrayBuffer::New(env, pcm.size());
    if (!pcm.empty()) std::memcpy(ab.Data(), pcm.data(), pcm.size());
    return ab;
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value FromPCM(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const uint8_t* data = nullptr;
    size_t len = 0;
    BytesArg(info, 0, &data, &len);
    const std::string fmtName = StrArgOrOpt(info, 1, "format", "f32");
    pal::PcmFormat fmt;
    if (!pal::ParsePcmFormat(fmtName, &fmt))
      throw std::invalid_argument("format must be u8|s16|s24|s32|f32|f64");
    const double channels = NumArg(info, 2, 0);
    const double rate = NumArg(info, 3, 0);
    pal::ValidateChannels((uint32_t)channels);
    pal::ValidateSampleRate((uint32_t)rate);
    std::vector<float> planar = pal::InterleavedToPlanar(
        data, len, (uint32_t)channels, pal::BytesPerPcmFormat(fmt),
        fmt == pal::PcmFormat::F32 || fmt == pal::PcmFormat::F64,
        fmt == pal::PcmFormat::U8);
    return NewHandle(env, std::make_unique<pal::AudioBuffer>(pal::AudioBuffer::FromPlanar(
                              std::move(planar), (uint32_t)channels, (uint32_t)rate)));
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value GetPeaks(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    const double buckets = NumArg(info, 1, 0);
    const double channel = NumArg(info, 2, 0);
    if (buckets < 1 || buckets > 1000000)
      throw std::invalid_argument("buckets must be between 1 and 1000000");
    if (channel < 0 || channel >= b->channels())
      throw std::invalid_argument("channel out of range");
    std::vector<float> peaks =
        pal::ComputePeaks(b->channel((size_t)channel), b->frames(), (size_t)buckets);
    Napi::Float32Array arr = Napi::Float32Array::New(env, peaks.size());
    if (!peaks.empty()) std::memcpy(arr.Data(), peaks.data(), peaks.size() * 4);
    return arr;
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value Analyze(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    Napi::Object out = Napi::Object::New(env);
    pal::LevelStats overall;
    Napi::Array chans = Napi::Array::New(env, b->channels());
    for (uint32_t c = 0; c < b->channels(); ++c) {
      pal::LevelStats s = pal::AnalyzeChannel(b->channel(c), b->frames());
      overall.peakLin = std::max(overall.peakLin, s.peakLin);
      overall.rmsLin = std::max(overall.rmsLin, s.rmsLin);
      chans[(uint32_t)c] = StatsToJs(env, s);
    }
    const auto db = [](double v) { return v > 1e-12 ? 20.0 * std::log10(v) : -INFINITY; };
    overall.peakDb = db(overall.peakLin);
    overall.rmsDb = db(overall.rmsLin);
    out.Set("channels", (double)b->channels());
    out.Set("sampleRate", (double)b->sampleRate());
    out.Set("frames", (double)b->frames());
    out.Set("durationSec", b->durationSec());
    out.Set("peakDb", overall.peakDb);
    out.Set("rmsDb", overall.rmsDb);
    out.Set("perChannel", chans);
    return out;
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value Gain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    const double gainDb = NumArg(info, 1, 0);
    const double fi = OptNum(info, 2, "fadeInSec", 0);
    const double fo = OptNum(info, 2, "fadeOutSec", 0);
    const std::string curve = OptStr(info, 2, "curve", "lin");
    if (curve != "lin" && curve != "smooth")
      throw std::invalid_argument("curve must be 'lin' or 'smooth'");
    return NewHandle(env, pal::ApplyGain(*b, gainDb, fi, fo, curve == "smooth"));
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value Normalize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    const double target = NumArg(info, 1, -0.1);
    return NewHandle(env, pal::Normalize(*b, target));
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value Fade(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    const double start = NumArg(info, 1, 0);
    const double dur = NumArg(info, 2, 0);
    const std::string dir = StrArgOrOpt(info, 3, "direction", "in");
    const std::string curve = StrArgOrOpt(info, 4, "curve", "lin");
    if (dir != "in" && dir != "out")
      throw std::invalid_argument("direction must be 'in' or 'out'");
    if (curve != "lin" && curve != "smooth")
      throw std::invalid_argument("curve must be 'lin' or 'smooth'");
    return NewHandle(
        env, pal::Fade(*b, start, dur, dir == "in", curve == "smooth"));
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value Pan(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    const double pan = ClampOpt(NumArg(info, 1, 0), -1.0, 1.0, "pan");
    return NewHandle(env, pal::Pan(*b, pan));
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value Reverse(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    return NewHandle(env, pal::Reverse(*b));
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value Biquad(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    const std::string typeName = OptStr(info, 1, "type", "peaking");
    pal::BiquadType type;
    if (!pal::ParseBiquadType(typeName, &type))
      throw std::invalid_argument(
            "type must be lowpass|highpass|bandpass|notch|allpass|peaking|lowshelf|highshelf");
    const double freq = OptNum(info, 1, "freqHz", 1000);
    const double q = OptNum(info, 1, "q", 0.7071);
    const double gainDb = OptNum(info, 1, "gainDb", 0);
    return NewHandle(env,
                     pal::ApplyBiquad(*b, pal::DesignBiquad(
                                             type, b->sampleRate(), freq, q, gainDb)));
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value Compressor(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    pal::CompressorParams p;
    p.thresholdDb = OptNum(info, 1, "thresholdDb", p.thresholdDb);
    p.ratio = OptNum(info, 1, "ratio", p.ratio);
    p.attackMs = OptNum(info, 1, "attackMs", p.attackMs);
    p.releaseMs = OptNum(info, 1, "releaseMs", p.releaseMs);
    p.kneeDb = OptNum(info, 1, "kneeDb", p.kneeDb);
    p.makeupDb = OptNum(info, 1, "makeupDb", p.makeupDb);
    return NewHandle(env, pal::Compress(*b, p));
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value Gate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    pal::GateParams p;
    p.thresholdDb = OptNum(info, 1, "thresholdDb", p.thresholdDb);
    p.attackMs = OptNum(info, 1, "attackMs", p.attackMs);
    p.releaseMs = OptNum(info, 1, "releaseMs", p.releaseMs);
    p.floorDb = OptNum(info, 1, "floorDb", p.floorDb);
    return NewHandle(env, pal::Gate(*b, p));
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value Reverb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    pal::ReverbParams p;
    p.roomSize = OptNum(info, 1, "roomSize", p.roomSize);
    p.damping = OptNum(info, 1, "damping", p.damping);
    p.wetLevel = OptNum(info, 1, "wetLevel", p.wetLevel);
    p.dryLevel = OptNum(info, 1, "dryLevel", p.dryLevel);
    p.width = OptNum(info, 1, "width", p.width);
    p.tailSec = OptNum(info, 1, "tailSec", p.tailSec);
    return NewHandle(env, pal::Reverb(*b, p));
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value DelayFx(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    pal::DelayParams p;
    p.delayMs = OptNum(info, 1, "delayMs", p.delayMs);
    p.feedback = OptNum(info, 1, "feedback", p.feedback);
    p.mix = OptNum(info, 1, "mix", p.mix);
    p.stereoSpreadMs = OptNum(info, 1, "stereoSpreadMs", p.stereoSpreadMs);
    return NewHandle(env, pal::Delay(*b, p));
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value SetChannels(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    const double ch = NumArg(info, 1, 0);
    pal::ValidateChannels((uint32_t)ch);
    return NewHandle(env, pal::ConvertChannels(*b, (uint32_t)ch));
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value Resample(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    const double rate = NumArg(info, 1, 0);
    pal::ValidateSampleRate((uint32_t)rate);
    const double quality = NumArg(info, 2, 1);
    return NewHandle(env, pal::Resample(*b, (uint32_t)rate, quality < 0.5 ? 0 : 1));
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value DetectSilence(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    const double thr = NumArg(info, 1, -45);
    const double minSec = NumArg(info, 2, 0.3);
    std::vector<pal::SilenceRegion> regions = pal::DetectSilence(*b, thr, minSec);
    Napi::Array arr = Napi::Array::New(env, regions.size());
    for (size_t i = 0; i < regions.size(); ++i) {
      Napi::Object o = Napi::Object::New(env);
      o.Set("startSec", regions[i].startSec);
      o.Set("endSec", regions[i].endSec);
      arr[(uint32_t)i] = o;
    }
    return arr;
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value RemoveSilence(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    const pal::AudioBuffer* b = BufArg(info, 0);
    if (!b) throw std::invalid_argument("invalid buffer handle");
    const double thr = OptNum(info, 1, "thresholdDb", -45);
    const double minSec = OptNum(info, 1, "minSilenceSec", 0.3);
    const double padMs = OptNum(info, 1, "padMs", 20);
    return NewHandle(env, pal::RemoveSilence(*b, thr, minSec, padMs));
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

Napi::Value Mix(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  try {
    if (info.Length() < 1 || !info[0].IsArray())
      throw std::invalid_argument("expected an array of tracks [{handle, gainDb, pan}]");
    Napi::Array tracks = info[0].As<Napi::Array>();
    std::vector<pal::MixTrack> mixTracks;
    for (uint32_t i = 0; i < tracks.Length(); ++i) {
      Napi::Value v = tracks[i];
      if (!v.IsObject()) throw std::invalid_argument("track entries must be objects");
      Napi::Object o = v.As<Napi::Object>();
      Napi::Value h = o.Get("handle");
      if (!h.IsNumber()) throw std::invalid_argument("track needs a numeric 'handle'");
      pal::AudioBuffer* buf = pal::Store().get(h.As<Napi::Number>().Uint32Value());
      if (!buf) throw std::invalid_argument("invalid buffer handle in track list");
      Napi::Value g = o.Get("gainDb");
      Napi::Value p = o.Get("pan");
      pal::MixTrack t;
      t.src = buf;
      t.gainLin = g.IsNumber() ? std::pow(10.0, g.As<Napi::Number>().DoubleValue() / 20.0) : 1.0;
      t.pan = p.IsNumber() ? p.As<Napi::Number>().DoubleValue() : 0.0;
      mixTracks.push_back(t);
    }
    const double outRate = NumArg(info, 1, 0);
    const double outChannels = NumArg(info, 2, 0);
    if (outRate != 0) pal::ValidateSampleRate((uint32_t)outRate);
    if (outChannels != 0) pal::ValidateChannels((uint32_t)outChannels);
    return NewHandle(env, pal::MixTracks(mixTracks, (uint32_t)outRate,
                                         (uint32_t)outChannels));
  } catch (const std::exception& e) { return ThrowCpp(env, e); }
}

}  // namespace

Napi::Object InitEngine(Napi::Env env, Napi::Object exports) {
  exports.Set("version", Napi::Function::New(env, Version));
  exports.Set("createBuffer", Napi::Function::New(env, CreateBuffer));
  exports.Set("freeBuffer", Napi::Function::New(env, FreeBuffer));
  exports.Set("info", Napi::Function::New(env, Info));
  exports.Set("stats", Napi::Function::New(env, Stats));
  exports.Set("loadWav", Napi::Function::New(env, LoadWav));
  exports.Set("writeWav", Napi::Function::New(env, WriteWav));
  exports.Set("generateTone", Napi::Function::New(env, GenerateTone));
  exports.Set("toPCM", Napi::Function::New(env, ToPCM));
  exports.Set("fromPCM", Napi::Function::New(env, FromPCM));
  exports.Set("getPeaks", Napi::Function::New(env, GetPeaks));
  exports.Set("analyze", Napi::Function::New(env, Analyze));
  exports.Set("gain", Napi::Function::New(env, Gain));
  exports.Set("normalize", Napi::Function::New(env, Normalize));
  exports.Set("fade", Napi::Function::New(env, Fade));
  exports.Set("pan", Napi::Function::New(env, Pan));
  exports.Set("reverse", Napi::Function::New(env, Reverse));
  exports.Set("biquad", Napi::Function::New(env, Biquad));
  exports.Set("compressor", Napi::Function::New(env, Compressor));
  exports.Set("gate", Napi::Function::New(env, Gate));
  exports.Set("reverb", Napi::Function::New(env, Reverb));
  exports.Set("delay", Napi::Function::New(env, DelayFx));
  exports.Set("setChannels", Napi::Function::New(env, SetChannels));
  exports.Set("resample", Napi::Function::New(env, Resample));
  exports.Set("detectSilence", Napi::Function::New(env, DetectSilence));
  exports.Set("removeSilence", Napi::Function::New(env, RemoveSilence));
  exports.Set("mix", Napi::Function::New(env, Mix));
  return exports;
}

NODE_API_MODULE(audio_engine, InitEngine)
