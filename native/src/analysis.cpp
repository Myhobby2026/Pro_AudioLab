// Pro AudioLab - native C++ audio engine
// analysis.cpp: peak/RMS/DC analysis and waveform peaks for UI rendering.
#include "analysis.h"

#include <cmath>

namespace pal {

LevelStats AnalyzeChannel(const float* x, size_t n) {
  LevelStats s;
  double sumSq = 0.0, sum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double v = x[i];
    const double a = std::fabs(v);
    if (a > s.peakLin) s.peakLin = a;
    sumSq += v * v;
    sum += v;
  }
  if (n > 0) {
    s.rmsLin = std::sqrt(sumSq / (double)n);
    s.dcOffset = sum / (double)n;
  }
  auto db = [](double v) { return v > 1e-12 ? 20.0 * std::log10(v) : -999.0; };
  s.peakDb = db(s.peakLin);
  s.rmsDb = db(s.rmsLin);
  return s;
}

std::vector<float> ComputePeaks(const float* x, size_t n, size_t buckets) {
  if (buckets == 0) throw std::invalid_argument("buckets must be >= 1");
  if (buckets > 1000000) buckets = 1000000;
  std::vector<float> peaks(buckets * 2, 0.0f);
  if (n == 0) return peaks;
  const double perBucket = (double)n / (double)buckets;
  for (size_t b = 0; b < buckets; ++b) {
    const size_t begin = (size_t)((double)b * perBucket);
    const size_t end = std::min((size_t)((double)(b + 1) * perBucket + 0.5), n);
    float lo = 0.0f, hi = 0.0f;
    if (begin < end) {
      lo = hi = x[begin];
      for (size_t i = begin + 1; i < end; ++i) {
        if (x[i] < lo) lo = x[i];
        if (x[i] > hi) hi = x[i];
      }
    }
    peaks[b * 2] = lo;
    peaks[b * 2 + 1] = hi;
  }
  return peaks;
}

}  // namespace pal
