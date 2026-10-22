#pragma once

#include "wlr_color.h"

#include <cstdint>

extern "C" {
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_output.h>
}

namespace umbriel {

  struct ColorLuminances {
    float min = 0.2F;
    float max = 80.0F;
    float reference = 80.0F;
  };

  struct ColorDescription {
    wlr_image_description_v1_data data{};
    ColorLuminances luminances;
    float luminanceMultiplier = 1.0F;
    bool requiresHdrOutput = false;
  };

  [[nodiscard]] ColorLuminances defaultColorLuminances(uint32_t transferFunction);
  [[nodiscard]] bool colorDescriptionsEqual(const ColorDescription& left, const ColorDescription& right);

  // The SDR reference white is adjustable only while composing into a PQ output.
  // Display metadata describes the target volume independently of the PQ encoding.
  [[nodiscard]] ColorDescription
  outputColorDescription(const wlr_output_image_description* outputDescription, float effectiveSdrWhite);

} // namespace umbriel
