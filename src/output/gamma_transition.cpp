#include "output/gamma_transition.h"

#include <cmath>

namespace umbriel {

  std::array<float, 3> hdrWhitePointFromGamma(const uint16_t* table, size_t rampSize) {
    std::array<float, 3> whitePoint{1.0F, 1.0F, 1.0F};
    if (!table || rampSize == 0) {
      return whitePoint;
    }

    for (size_t channel = 0; channel < whitePoint.size(); ++channel) {
      const uint16_t* ramp = table + channel * rampSize;
      bool inclusiveIdentity = rampSize > 1;
      bool steppedIdentity = rampSize > 1;
      for (size_t i = 0; i < rampSize && (inclusiveIdentity || steppedIdentity); ++i) {
        const double inclusiveSample = std::floor(65535.0 * static_cast<double>(i) / (rampSize - 1));
        const double steppedSample = std::floor(65536.0 * static_cast<double>(i) / rampSize);
        inclusiveIdentity = inclusiveIdentity && std::abs(ramp[i] - inclusiveSample) <= 1.0;
        steppedIdentity = steppedIdentity && std::abs(ramp[i] - steppedSample) <= 1.0;
      }
      // Some clients sample identity with i / N, so its last entry is below white.
      // Match the complete ramp before treating that endpoint as neutral.
      whitePoint[channel] =
          inclusiveIdentity || steppedIdentity ? 1.0F : static_cast<float>(ramp[rampSize - 1]) / 65535.0F;
    }
    return whitePoint;
  }

  bool stageGammaForOutputMode(GammaOutputMode mode, const GammaTransitionOps& ops) {
    if (mode == GammaOutputMode::Hdr) {
      ops.clear();
      return true;
    }
    if (ops.apply()) {
      return true;
    }
    ops.clear();
    return false;
  }

} // namespace umbriel
