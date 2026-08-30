// Pro AudioLab - native C++ audio engine
// dsp.cpp: biquad filters (RBJ cookbook), compressor, noise gate,
// Freeverb-style reverb, feedback delay, gain/fades, pan, resampling,
// channel conversion and silence tools.
#include "dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <stdexcept>

namespace pal {

namespace {

constexpr double kPi = 3.14159265358979323846;
inline double ClampD(double v, double lo, double hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
inline double DbToLin(double db) { return std::pow(10.0, db / 20.0); }
inline double LinToDb(double v) { return v > 0.0 ? 20.0 * std::log10(v) : -999.0; }

// ------------------------------------------------------------ freeverb bits
struct Comb {
  std::vector<float> buf;
  size_t idx = 0;
  double filterstore = 0.0;
  void init(size_t n) { buf.assign(n, 0.0f); idx = 0; filterstore = 0.0; }
  double process(double in, double feedback, double damp1, double damp2) {
    const double output = buf[idx];
    filterstore = output * damp2 + filterstore * damp1;
    buf[idx] = (float)(in + filterstore * feedback);
    if (++idx >= buf.size()) idx = 0;
    return output;
  }
};

struct Allpass {
  std::vector<float> buf;
  size_t idx = 0;
  void init(size_t n) { buf.assign(n, 0.0f); idx = 0; }
  double process(double in) {
    const double bufout = buf[idx];
    const double output = -in + bufout;
    buf[idx] = (float)(in + bufout * 0.5);
    if (++idx >= buf.size()) idx = 0;
    return output;
  }
};

constexpr int kNumCombs = 8;
constexpr int kNumAllpass = 4;
constexpr int kCombTuning[kNumCombs] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr int kAllpassTuning[kNumAllpass] = {556, 441, 341, 225};
constexpr double kStereoSpread = 23.0;
constexpr double kFixedGain = 0.015;
constexpr double kScaleWet = 3.0;
constexpr double kScaleDry = 2.0;
constexpr double kScaleDamp = 0.4;
constexpr double kScaleRoom = 0.28;
constexpr double kOffsetRoom = 0.7;

struct ReverbChain {
  Comb combs[kNumCombs];
  Allpass allpass[kNumAllpass];

  void init(double fs, double spreadSamples) {
    for (int i = 0; i < kNumCombs; ++i) {
      size_t n = (size_t)std::llround((kCombTuning[i] + spreadSamples) * fs / 44100.0);
      combs[i].init(std::max<size_t>(n, 4));
    }
    for (int i = 0; i < kNumAllpass; ++i) {
      size_t n = (size_t)std::llround((kAllpassTuning[i] + spreadSamples) * fs / 44100.0);
      allpass[i].init(std::max<size_t>(n, 4));
    }
  }
  double process(double in, double feedback, double damp1, double damp2) {
    double out = 0.0;
    const double input = in * kFixedGain;
    for (int i = 0; i < kNumCombs; ++i) out += combs[i].process(input, feedback, damp1, damp2);
    for (int i = 0; i < kNumAllpass; ++i) out = allpass[i].process(out);
    return out;
  }
};

}  // namespace

// ------------------------------------------------------------------ biquad
bool ParseBiquadType(const std::string& s, BiquadType* out) {
  if (s == "lowpass" || s == "lp") *out = BiquadType::LowPass;
  else if (s == "highpass" || s == "hp") *out = BiquadType::HighPass;
  else if (s == "bandpass" || s == "bp") *out = BiquadType::BandPass;
  else if (s == "notch") *out = BiquadType::Notch;
  else if (s == "allpass" || s == "ap") *out = BiquadType::AllPass;
  else if (s == "peaking" || s == "peak" || s == "bell") *out = BiquadType::Peaking;
  else if (s == "lowshelf" || s == "low_shelf") *out = BiquadType::LowShelf;
  else if (s == "highshelf" || s == "high_shelf") *out = BiquadType::HighShelf;
  else return false;
  return true;
}

BiquadCoeffs DesignBiquad(BiquadType type, double sampleRate, double freqHz,
                          double q, double gainDb) {
  if (sampleRate <= 0) throw std::invalid_argument("bad sample rate");
  if (freqHz <= 0 || freqHz >= sampleRate * 0.5)
    throw std::invalid_argument("freqHz must be between 0 and Nyquist (sampleRate/2)");
  q = ClampD(q, 0.05, 20.0);
  if (gainDb < -40 || gainDb > 40)
    throw std::invalid_argument("gainDb must be between -40 and +40");

  const double A = std::pow(10.0, gainDb / 40.0);
  const double w0 = 2.0 * kPi * freqHz / sampleRate;
  const double cw = std::cos(w0);
  const double sw = std::sin(w0);
  const double alpha = sw / (2.0 * q);
  const double sqA2a = 2.0 * std::sqrt(A) * alpha;

  double b0, b1, b2, a0, a1, a2;
  switch (type) {
    case BiquadType::LowPass:
      b0 = (1.0 - cw) / 2.0; b1 = 1.0 - cw; b2 = (1.0 - cw) / 2.0;
      a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
      break;
    case BiquadType::HighPass:
      b0 = (1.0 + cw) / 2.0; b1 = -(1.0 + cw); b2 = (1.0 + cw) / 2.0;
      a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
      break;
    case BiquadType::BandPass:
      b0 = alpha; b1 = 0.0; b2 = -alpha;
      a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
      break;
    case BiquadType::Notch:
      b0 = 1.0; b1 = -2.0 * cw; b2 = 1.0;
      a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
      break;
    case BiquadType::AllPass:
      b0 = 1.0 - alpha; b1 = -2.0 * cw; b2 = 1.0 + alpha;
      a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
      break;
    case BiquadType::Peaking:
      b0 = 1.0 + alpha * A; b1 = -2.0 * cw; b2 = 1.0 - alpha * A;
      a0 = 1.0 + alpha / A; a1 = -2.0 * cw; a2 = 1.0 - alpha / A;
      break;
    case BiquadType::LowShelf:
      b0 = A * ((A + 1.0) - (A - 1.0) * cw + sqA2a);
      b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
      b2 = A * ((A + 1.0) - (A - 1.0) * cw - sqA2a);
      a0 = (A + 1.0) + (A - 1.0) * cw + sqA2a;
      a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw);
      a2 = (A + 1.0) + (A - 1.0) * cw - sqA2a;
      break;
    case BiquadType::HighShelf:
      b0 = A * ((A + 1.0) + (A - 1.0) * cw + sqA2a);
      b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
      b2 = A * ((A + 1.0) + (A - 1.0) * cw - sqA2a);
      a0 = (A + 1.0) - (A - 1.0) * cw + sqA2a;
      a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
      a2 = (A + 1.0) - (A - 1.0) * cw - sqA2a;
      break;
    default:
      throw std::invalid_argument("unknown filter type");
  }
  if (a0 == 0.0) throw std::invalid_argument("degenerate filter coefficients");
  return BiquadCoeffs{b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

std::unique_ptr<AudioBuffer> ApplyBiquad(const AudioBuffer& in,
                                         const BiquadCoeffs& c) {
  const size_t frames = in.frames();
  auto out = std::make_unique<AudioBuffer>(frames, in.channels(), in.sampleRate());
  for (uint32_t ch = 0; ch < in.channels(); ++ch) {
    const float* x = in.channel(ch);
    float* y = out->channel(ch);
    // Transposed direct form II
    double z1 = 0.0, z2 = 0.0;
    for (size_t i = 0; i < frames; ++i) {
      const double xn = x[i];
      const double yn = c.b0 * xn + z1;
      z1 = c.b1 * xn - c.a1 * yn + z2;
      z2 = c.b2 * xn - c.a2 * yn;
      y[i] = (float)yn;
    }
  }
  return out;
}

// ------------------------------------------------------------- gain / fades
std::unique_ptr<AudioBuffer> ApplyGain(const AudioBuffer& in, double gainDb,
                                       double fadeInSec, double fadeOutSec,
                                       bool smoothCurve) {
  const double g = DbToLin(ClampD(gainDb, -120.0, 48.0));
  const size_t frames = in.frames();
  const uint32_t fs = in.sampleRate();
  size_t fi = 0, fo = 0;
  if (fadeInSec > 0 && fs > 0) fi = (size_t)std::llround(fadeInSec * fs);
  if (fadeOutSec > 0 && fs > 0) fo = (size_t)std::llround(fadeOutSec * fs);
  if (fi > frames) fi = frames;
  if (fo > frames) fo = frames;

  auto out = std::make_unique<AudioBuffer>(frames, in.channels(), fs);
  for (size_t i = 0; i < frames; ++i) {
    double f = 1.0;
    if (i < fi) {
      const double t = fi ? (double)(i + 1) / (double)fi : 1.0;
      f = smoothCurve ? t * t * t : t;
    }
    if (fo > 0 && i + fo >= frames) {
      const double rem = (double)(frames - i);  // 1..fo
      const double t = rem / (double)fo;
      f = std::min(f, smoothCurve ? t * t * t : t);
    }
    const double mul = g * f;
    for (uint32_t ch = 0; ch < in.channels(); ++ch)
      out->channel(ch)[i] = (float)(in.channel(ch)[i] * mul);
  }
  return out;
}

std::unique_ptr<AudioBuffer> Normalize(const AudioBuffer& in,
                                       double targetPeakDb) {
  double peak = 0.0;
  for (uint32_t ch = 0; ch < in.channels(); ++ch) {
    const float* x = in.channel(ch);
    for (size_t i = 0; i < in.frames(); ++i)
      peak = std::max<double>(peak, std::fabs(x[i]));
  }
  if (peak <= 0.0) {  // silence: return a copy
    return std::make_unique<AudioBuffer>(in.frames(), in.channels(), in.sampleRate());
  }
  const double target = DbToLin(ClampD(targetPeakDb, -60.0, 0.0));
  const double g = target / peak;
  auto out = std::make_unique<AudioBuffer>(in.frames(), in.channels(), in.sampleRate());
  for (uint32_t ch = 0; ch < in.channels(); ++ch) {
    const float* x = in.channel(ch);
    float* y = out->channel(ch);
    for (size_t i = 0; i < in.frames(); ++i) y[i] = (float)(x[i] * g);
  }
  return out;
}

std::unique_ptr<AudioBuffer> Fade(const AudioBuffer& in, double startSec,
                                  double durSec, bool fadeIn,
                                  bool smoothCurve) {
  auto out = std::make_unique<AudioBuffer>(in.frames(), in.channels(), in.sampleRate());
  for (uint32_t ch = 0; ch < in.channels(); ++ch)
    std::copy(in.channel(ch), in.channel(ch) + in.frames(), out->channel(ch));

  if (durSec <= 0 || in.sampleRate() == 0) return out;
  const uint32_t fs = in.sampleRate();
  const double start = std::max(0.0, std::floor(startSec * (double)fs));
  size_t s = (size_t)start;
  size_t n = (size_t)std::llround(durSec * fs);
  if (s >= out->frames()) return out;
  if (n == 0) return out;
  if (s + n > out->frames()) n = out->frames() - s;

  for (uint32_t ch = 0; ch < out->channels(); ++ch) {
    float* x = out->channel(ch);
    for (size_t k = 0; k < n; ++k) {
      const double t = (double)(k + 1) / (double)n;  // 0..1
      const double f = smoothCurve ? t * t * t : t;
      const double w = fadeIn ? f : (1.0 - f);
      x[s + k] = (float)(x[s + k] * w);
    }
  }
  return out;
}

std::unique_ptr<AudioBuffer> Pan(const AudioBuffer& in, double pan) {
  pan = ClampD(pan, -1.0, 1.0);
  if (in.channels() == 1) {
    // mono -> stereo equal power
    const double theta = (pan + 1.0) * kPi / 4.0;
    const double lg = std::cos(theta), rg = std::sin(theta);
    auto out = std::make_unique<AudioBuffer>(in.frames(), 2, in.sampleRate());
    for (size_t i = 0; i < in.frames(); ++i) {
      out->channel(0)[i] = (float)(in.channel(0)[i] * lg);
      out->channel(1)[i] = (float)(in.channel(0)[i] * rg);
    }
    return out;
  }
  // multi-channel balance law: attenuate the side opposite to the pan
  auto out = std::make_unique<AudioBuffer>(in.frames(), in.channels(), in.sampleRate());
  for (uint32_t ch = 0; ch < in.channels(); ++ch)
    std::copy(in.channel(ch), in.channel(ch) + in.frames(), out->channel(ch));
  const double att = std::cos(std::fabs(pan) * kPi / 2.0);
  if (pan < 0 && out->channels() >= 2) {
    float* r = out->channel(1);
    for (size_t i = 0; i < out->frames(); ++i) r[i] = (float)(r[i] * att);
  } else if (pan > 0) {
    float* l = out->channel(0);
    for (size_t i = 0; i < out->frames(); ++i) l[i] = (float)(l[i] * att);
  }
  return out;
}

std::unique_ptr<AudioBuffer> Reverse(const AudioBuffer& in) {
  auto out = std::make_unique<AudioBuffer>(in.frames(), in.channels(), in.sampleRate());
  for (uint32_t ch = 0; ch < in.channels(); ++ch) {
    const float* x = in.channel(ch);
    float* y = out->channel(ch);
    const size_t n = in.frames();
    for (size_t i = 0; i < n; ++i) y[i] = x[n - 1 - i];
  }
  return out;
}

// ---------------------------------------------------------------- dynamics
std::unique_ptr<AudioBuffer> Compress(const AudioBuffer& in,
                                      const CompressorParams& p) {
  const uint32_t fs = in.sampleRate();
  if (fs == 0) throw std::invalid_argument("buffer has no sample rate");
  CompressorParams c = p;
  c.ratio = ClampD(c.ratio, 1.0, 100.0);
  c.thresholdDb = ClampD(c.thresholdDb, -96.0, 0.0);
  c.kneeDb = ClampD(std::fabs(c.kneeDb), 0.0, 24.0);
  c.attackMs = ClampD(c.attackMs, 0.01, 1000.0);
  c.releaseMs = ClampD(c.releaseMs, 1.0, 5000.0);
  c.makeupDb = ClampD(c.makeupDb, -24.0, 24.0);

  const double att = std::exp(-1.0 / (c.attackMs * 0.001 * fs));
  const double rel = std::exp(-1.0 / (c.releaseMs * 0.001 * fs));
  const double thr = c.thresholdDb;
  const double w = c.kneeDb;
  const double slope = 1.0 / c.ratio - 1.0;  // negative

  auto out = std::make_unique<AudioBuffer>(in.frames(), in.channels(), fs);
  for (uint32_t ch = 0; ch < in.channels(); ++ch) {
    const float* x = in.channel(ch);
    float* y = out->channel(ch);
    double env = 0.0;
    for (size_t i = 0; i < in.frames(); ++i) {
      const double xl = std::fabs(x[i]);
      const double coeff = xl > env ? att : rel;
      env += coeff * (xl - env);
      const double envDb = env > 1e-9 ? LinToDb(env) : -999.0;
      const double over = envDb - thr;
      double gr = 0.0;  // gain reduction in dB (<= 0)
      if (w > 0.0 && over > -w / 2.0 && over < w / 2.0) {
        const double t = over + w / 2.0;
        gr = slope * t * t / (2.0 * w);
      } else if (over >= w / 2.0) {
        gr = slope * over;
      }
      const double gainDb = gr + c.makeupDb;
      y[i] = (float)(x[i] * DbToLin(gainDb));
    }
  }
  return out;
}

std::unique_ptr<AudioBuffer> Gate(const AudioBuffer& in, const GateParams& p) {
  const uint32_t fs = in.sampleRate();
  if (fs == 0) throw std::invalid_argument("buffer has no sample rate");
  GateParams g = p;
  g.thresholdDb = ClampD(g.thresholdDb, -96.0, 0.0);
  g.attackMs = ClampD(g.attackMs, 0.01, 1000.0);
  g.releaseMs = ClampD(g.releaseMs, 1.0, 5000.0);
  g.floorDb = ClampD(g.floorDb, -120.0, 0.0);

  const double att = std::exp(-1.0 / (g.attackMs * 0.001 * fs));
  const double rel = std::exp(-1.0 / (g.releaseMs * 0.001 * fs));
  const double openLin = DbToLin(g.thresholdDb);
  const double closeLin = openLin * 0.5;  // hysteresis: closes 6 dB lower
  const double floorLin = g.floorDb <= -119.0 ? 0.0 : DbToLin(g.floorDb);

  auto out = std::make_unique<AudioBuffer>(in.frames(), in.channels(), fs);
  for (uint32_t ch = 0; ch < in.channels(); ++ch) {
    const float* x = in.channel(ch);
    float* y = out->channel(ch);
    double env = 0.0, gain = 1.0;
    bool open = true;
    for (size_t i = 0; i < in.frames(); ++i) {
      const double xl = std::fabs(x[i]);
      const double coeff = xl > env ? att : rel;
      env += coeff * (xl - env);
      if (open && env < closeLin) open = false;
      else if (!open && env >= openLin) open = true;
      const double target = open ? 1.0 : floorLin;
      const double gcoeff = target > gain ? att : rel;
      gain += gcoeff * (target - gain);
      y[i] = (float)(x[i] * gain);
    }
  }
  return out;
}

// ------------------------------------------------------------------ reverb
std::unique_ptr<AudioBuffer> Reverb(const AudioBuffer& in,
                                    const ReverbParams& p) {
  const uint32_t fs = in.sampleRate();
  if (fs == 0) throw std::invalid_argument("buffer has no sample rate");
  ReverbParams r = p;
  r.roomSize = ClampD(r.roomSize, 0.0, 1.0);
  r.damping = ClampD(r.damping, 0.0, 1.0);
  r.wetLevel = ClampD(r.wetLevel, 0.0, 1.0);
  r.dryLevel = ClampD(r.dryLevel, 0.0, 1.0);
  r.width = ClampD(r.width, 0.0, 1.0);
  r.tailSec = ClampD(r.tailSec, 0.0, 30.0);

  const double feedback = r.roomSize * kScaleRoom + kOffsetRoom;
  const double damp = r.damping * kScaleDamp;
  const double damp1 = 1.0 - damp, damp2 = damp;
  const double wet = r.wetLevel * kScaleWet;
  const double dry = r.dryLevel * kScaleDry;
  const double wet1 = wet * (1.0 + r.width * 0.5);
  const double wet2 = wet * (1.0 - r.width * 0.5);

  const size_t inFrames = in.frames();
  const size_t tail = (size_t)std::llround(r.tailSec * fs);
  const size_t total = inFrames + tail;

  ReverbChain chainL, chainR;
  chainL.init(fs, 0.0);
  chainR.init(fs, kStereoSpread);

  const float* inL = in.channel(0);
  const float* inR = in.channels() >= 2 ? in.channel(1) : in.channel(0);

  auto out = std::make_unique<AudioBuffer>(total, 2, fs);
  float* outL = out->channel(0);
  float* outR = out->channel(1);

  for (size_t i = 0; i < total; ++i) {
    const double xl = i < inFrames ? (double)inL[i] : 0.0;
    const double xr = i < inFrames ? (double)inR[i] : 0.0;
    const double wetOutL = chainL.process(xl, feedback, damp1, damp2);
    const double wetOutR = chainR.process(xr, feedback, damp1, damp2);
    outL[i] = (float)(wet1 * wetOutL + dry * xl);
    outR[i] = (float)(wet2 * wetOutR + dry * xr);
  }
  return out;
}

// ------------------------------------------------------------------- delay
std::unique_ptr<AudioBuffer> Delay(const AudioBuffer& in,
                                   const DelayParams& p) {
  const uint32_t fs = in.sampleRate();
  if (fs == 0) throw std::invalid_argument("buffer has no sample rate");
  DelayParams d = p;
  d.delayMs = ClampD(d.delayMs, 0.1, 10000.0);
  d.feedback = ClampD(d.feedback, 0.0, 0.95);
  d.mix = ClampD(d.mix, 0.0, 1.0);
  d.stereoSpreadMs = ClampD(d.stereoSpreadMs, 0.0, 1000.0);

  const size_t frames = in.frames();
  auto out = std::make_unique<AudioBuffer>(frames, in.channels(), fs);

  for (uint32_t ch = 0; ch < in.channels(); ++ch) {
    const double ms = d.delayMs + (ch == 1 ? d.stereoSpreadMs : 0.0);
    const size_t len = std::max<size_t>((size_t)std::llround(ms * fs / 1000.0), 1);
    std::vector<float> tape(len, 0.0f);
    size_t pos = 0;
    const float* x = in.channel(ch);
    float* y = out->channel(ch);
    for (size_t i = 0; i < frames; ++i) {
      const float delayed = tape[pos];
      const float write = x[i] + delayed * (float)d.feedback;
      tape[pos] = write;
      if (++pos >= len) pos = 0;
      y[i] = (float)(x[i] * (1.0 - d.mix) + delayed * d.mix);
    }
  }
  return out;
}

// -------------------------------------------------- channels & sample rate
std::unique_ptr<AudioBuffer> ConvertChannels(const AudioBuffer& in,
                                             uint32_t targetChannels) {
  ValidateChannels(targetChannels);
  const uint32_t src = in.channels();
  if (src == targetChannels) {
    return std::make_unique<AudioBuffer>(in.frames(), src, in.sampleRate());
  }
  const size_t frames = in.frames();
  auto out = std::make_unique<AudioBuffer>(frames, targetChannels, in.sampleRate());

  if (src == 1) {  // duplicate mono to all channels
    for (uint32_t c = 0; c < targetChannels; ++c)
      std::copy(in.channel(0), in.channel(0) + frames, out->channel(c));
  } else if (targetChannels == 1) {  // mix down
    float* m = out->channel(0);
    const double inv = 1.0 / src;
    for (size_t i = 0; i < frames; ++i) {
      double acc = 0.0;
      for (uint32_t c = 0; c < src; ++c) acc += in.channel(c)[i];
      m[i] = (float)(acc * inv);
    }
  } else if (targetChannels < src) {  // grouped downmix
    for (uint32_t c = 0; c < targetChannels; ++c) {
      const uint32_t begin = (uint32_t)((uint64_t)c * src / targetChannels);
      const uint32_t end = (uint32_t)((uint64_t)(c + 1) * src / targetChannels);
      const uint32_t n = std::max<uint32_t>(end - begin, 1);
      const double inv = 1.0 / n;
      float* y = out->channel(c);
      for (size_t i = 0; i < frames; ++i) {
        double acc = 0.0;
        for (uint32_t k = begin; k < begin + n && k < src; ++k)
          acc += in.channel(k)[i];
        y[i] = (float)(acc * inv);
      }
    }
  } else {  // upmix by cycling sources
    for (uint32_t c = 0; c < targetChannels; ++c) {
      const uint32_t s = c % src;
      std::copy(in.channel(s), in.channel(s) + frames, out->channel(c));
    }
  }
  return out;
}

std::unique_ptr<AudioBuffer> Resample(const AudioBuffer& in,
                                      uint32_t targetRate, int quality) {
  ValidateSampleRate(targetRate);
  if (targetRate == in.sampleRate()) {
    return std::make_unique<AudioBuffer>(in.frames(), in.channels(), in.sampleRate());
  }
  const uint32_t srcRate = in.sampleRate();
  const size_t inFrames = in.frames();
  const size_t outFrames =
      (size_t)std::max<int64_t>(0, std::llround((double)inFrames * targetRate / srcRate));
  auto out = std::make_unique<AudioBuffer>(outFrames, in.channels(), targetRate);
  const double step = (double)srcRate / (double)targetRate;

  for (uint32_t ch = 0; ch < in.channels(); ++ch) {
    const float* x = in.channel(ch);
    float* y = out->channel(ch);
    for (size_t i = 0; i < outFrames; ++i) {
      const double pos = (double)i * step;
      const size_t i0 = (size_t)pos;
      const double frac = pos - (double)i0;
      if (quality <= 0 || inFrames < 4) {
        const size_t i1 = std::min(i0 + 1, inFrames ? inFrames - 1 : 0);
        y[i] = inFrames ? (float)((double)x[i0] * (1.0 - frac) + (double)x[i1] * frac) : 0.0f;
      } else {
        // Hermite 4-point interpolation
        const size_t im1 = i0 > 0 ? i0 - 1 : 0;
        const size_t i1 = std::min(i0 + 1, inFrames - 1);
        const size_t i2 = std::min(i0 + 2, inFrames - 1);
        const double xm1 = x[im1], x0 = x[i0], x1 = x[i1], x2 = x[i2];
        const double c0 = x0;
        const double c1 = 0.5 * (x1 - xm1);
        const double c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2;
        const double c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
        const double t = frac;
        y[i] = (float)(((c3 * t + c2) * t + c1) * t + c0);
      }
    }
  }
  return out;
}

// ---------------------------------------------------------------- silence
namespace {
std::vector<uint8_t> BuildSilentBlocks(const AudioBuffer& in, double thresholdDb,
                                       size_t* blockFramesOut) {
  const uint32_t fs = in.sampleRate();
  const size_t block = std::max<size_t>((size_t)std::llround(0.01 * fs), 1);  // 10 ms
  const size_t nBlocks = (in.frames() + block - 1) / block;
  if (blockFramesOut) *blockFramesOut = block;
  std::vector<uint8_t> silent(nBlocks, 1);
  const double thr = DbToLin(ClampD(thresholdDb, -120.0, 0.0));
  for (uint32_t ch = 0; ch < in.channels(); ++ch) {
    const float* x = in.channel(ch);
    for (size_t b = 0; b < nBlocks; ++b) {
      if (!silent[b]) continue;
      const size_t begin = b * block;
      const size_t end = std::min(begin + block, in.frames());
      for (size_t i = begin; i < end; ++i) {
        if (std::fabs(x[i]) >= thr) { silent[b] = 0; break; }
      }
    }
  }
  return silent;
}
}  // namespace

std::vector<SilenceRegion> DetectSilence(const AudioBuffer& in,
                                         double thresholdDb,
                                         double minSilenceSec) {
  std::vector<SilenceRegion> regions;
  if (in.frames() == 0 || in.sampleRate() == 0) return regions;
  minSilenceSec = std::max(0.0, minSilenceSec);
  size_t block;
  const std::vector<uint8_t> silent = BuildSilentBlocks(in, thresholdDb, &block);
  const uint32_t fs = in.sampleRate();
  const double blockSec = (double)block / fs;

  size_t runStart = 0;
  bool inRun = false;
  for (size_t b = 0; b <= silent.size(); ++b) {
    const bool isSilent = b < silent.size() ? silent[b] != 0 : false;
    if (isSilent && !inRun) { inRun = true; runStart = b; }
    else if (!isSilent && inRun) {
      inRun = false;
      const double startSec = runStart * blockSec;
      const double endSec = b * blockSec;
      if (endSec - startSec >= minSilenceSec)
        regions.push_back({startSec, endSec});
    }
  }
  return regions;
}

std::unique_ptr<AudioBuffer> RemoveSilence(const AudioBuffer& in,
                                           double thresholdDb,
                                           double minSilenceSec, double padMs) {
  const uint32_t fs = in.sampleRate();
  if (in.frames() == 0 || fs == 0)
    return std::make_unique<AudioBuffer>(in.frames(), in.channels(), fs);

  size_t block;
  std::vector<uint8_t> silent = BuildSilentBlocks(in, thresholdDb, &block);
  minSilenceSec = std::max(0.0, minSilenceSec);
  padMs = ClampD(padMs, 0.0, 1000.0);
  const size_t padBlocks = (size_t)std::llround(padMs * 0.001 * fs / (double)block);

  // A run of silent blocks is removed only when it is long enough.
  std::vector<uint8_t> keep(silent.size(), 1);
  size_t runStart = 0;
  bool inRun = false;
  auto endRun = [&](size_t endExclusive) {
    const double runSec = (double)(endExclusive - runStart) * block / fs;
    if (runSec >= minSilenceSec && minSilenceSec > 0)
      for (size_t b = runStart; b < endExclusive; ++b) keep[b] = 0;
  };
  for (size_t b = 0; b <= silent.size(); ++b) {
    const bool isSilent = b < silent.size() ? silent[b] != 0 : false;
    if (isSilent && !inRun) { inRun = true; runStart = b; }
    else if (!isSilent && inRun) { inRun = false; endRun(b); }
  }

  // Keep a bit of context (pad) around the remaining audio.
  std::vector<uint8_t> keepPadded = keep;
  for (size_t b = 0; b < keep.size(); ++b) {
    if (!keep[b]) continue;
    for (size_t k = 1; k <= padBlocks; ++k) {
      if (b >= k) keepPadded[b - k] = 1;
      if (b + k < keep.size()) keepPadded[b + k] = 1;
    }
  }

  size_t outFrames = 0;
  for (size_t b = 0; b < keepPadded.size(); ++b)
    if (keepPadded[b]) outFrames += std::min(block, in.frames() - b * block);

  auto out = std::make_unique<AudioBuffer>(outFrames, in.channels(), fs);
  size_t w = 0;
  for (size_t b = 0; b < keepPadded.size(); ++b) {
    if (!keepPadded[b]) continue;
    const size_t begin = b * block;
    const size_t n = std::min(block, in.frames() - begin);
    for (uint32_t ch = 0; ch < in.channels(); ++ch)
      std::copy(in.channel(ch) + begin, in.channel(ch) + begin + n,
                out->channel(ch) + w);
    w += n;
  }
  return out;
}

}  // namespace pal
