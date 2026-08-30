// Pro AudioLab - native C++ audio engine
// dsp.h: effects, gain staging, resampling, channel conversion, silence tools.
#pragma once

#include "buffer.h"

#include <memory>
#include <vector>

namespace pal {

// ---------------------------------------------------------------- biquad EQ
enum class BiquadType {
  LowPass, HighPass, BandPass, Notch, AllPass, Peaking, LowShelf, HighShelf
};
bool ParseBiquadType(const std::string& s, BiquadType* out);

struct BiquadCoeffs { double b0, b1, b2, a1, a2; };
// RBJ audio-EQ-cookbook design. Throws on invalid parameters.
BiquadCoeffs DesignBiquad(BiquadType type, double sampleRate, double freqHz,
                          double q, double gainDb);
std::unique_ptr<AudioBuffer> ApplyBiquad(const AudioBuffer& in,
                                         const BiquadCoeffs& c);

// ------------------------------------------------------------- gain / fades
// gainDb applied to whole buffer; optional fade-in/out; curve: true = smooth
// (cubic) instead of linear.
std::unique_ptr<AudioBuffer> ApplyGain(const AudioBuffer& in, double gainDb,
                                       double fadeInSec, double fadeOutSec,
                                       bool smoothCurve);
std::unique_ptr<AudioBuffer> Normalize(const AudioBuffer& in,
                                       double targetPeakDb);
std::unique_ptr<AudioBuffer> Fade(const AudioBuffer& in, double startSec,
                                  double durSec, bool fadeIn,
                                  bool smoothCurve);

// Pan. Mono input -> stereo (equal-power). Stereo/multi: balance law
// (attenuate the opposite side; no centre dip).
std::unique_ptr<AudioBuffer> Pan(const AudioBuffer& in, double pan);
std::unique_ptr<AudioBuffer> Reverse(const AudioBuffer& in);

// ------------------------------------------------------------- dynamics
struct CompressorParams {
  double thresholdDb = -20.0;
  double ratio = 4.0;
  double attackMs = 10.0;
  double releaseMs = 100.0;
  double kneeDb = 6.0;
  double makeupDb = 0.0;
};
std::unique_ptr<AudioBuffer> Compress(const AudioBuffer& in,
                                      const CompressorParams& p);

struct GateParams {
  double thresholdDb = -45.0;
  double attackMs = 1.0;
  double releaseMs = 60.0;
  double floorDb = -96.0;
};
std::unique_ptr<AudioBuffer> Gate(const AudioBuffer& in, const GateParams& p);

// ---------------------------------------------------------------- reverb
// Freeverb-style Schroeder reverb (8 combs + 4 allpasses per side).
// Always renders stereo output from the first one/two input channels.
// tailSec of decay tail is appended after the input ends.
struct ReverbParams {
  double roomSize = 0.7;   // 0..1
  double damping = 0.35;   // 0..1
  double wetLevel = 0.3;   // 0..1
  double dryLevel = 0.7;   // 0..1
  double width = 1.0;      // 0..1 stereo width
  double tailSec = 1.0;    // extra decay tail
};
std::unique_ptr<AudioBuffer> Reverb(const AudioBuffer& in,
                                    const ReverbParams& p);

// ----------------------------------------------------------------- delay
struct DelayParams {
  double delayMs = 300.0;
  double feedback = 0.35;       // 0..0.95
  double mix = 0.35;            // wet level 0..1
  double stereoSpreadMs = 0.0;  // extra delay on the right channel
};
std::unique_ptr<AudioBuffer> Delay(const AudioBuffer& in, const DelayParams& p);

// -------------------------------------------------- channels & sample rate
std::unique_ptr<AudioBuffer> ConvertChannels(const AudioBuffer& in,
                                             uint32_t targetChannels);
// quality: 0 = linear, 1 = hermite (cubic)
std::unique_ptr<AudioBuffer> Resample(const AudioBuffer& in,
                                      uint32_t targetRate, int quality);

// ---------------------------------------------------------------- silence
struct SilenceRegion { double startSec; double endSec; };
std::vector<SilenceRegion> DetectSilence(const AudioBuffer& in,
                                         double thresholdDb,
                                         double minSilenceSec);
// Removes silence runs longer than minSilenceSec (level < thresholdDb),
// keeping padMs of context around the remaining audio.
std::unique_ptr<AudioBuffer> RemoveSilence(const AudioBuffer& in,
                                           double thresholdDb,
                                           double minSilenceSec, double padMs);

}  // namespace pal
