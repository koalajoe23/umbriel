#include "server/color_description.h"

#include "check.h"

#include <array>

namespace {

  wlr_output_image_description hdrOutputDescription() {
    return {
        .primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020,
        .transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
        .mastering_display_primaries =
            {
                .red = {.x = 0.68F, .y = 0.32F},
                .green = {.x = 0.265F, .y = 0.69F},
                .blue = {.x = 0.15F, .y = 0.06F},
                .white = {.x = 0.3127F, .y = 0.329F},
            },
        .mastering_luminance = {.min = 0.01, .max = 1000.0},
        .max_cll = 1000.0,
        .max_fall = 400.0,
    };
  }

} // namespace

UMBRIEL_TEST(hdrReferenceWhiteChangesIndependentlyOfDisplayMetadata) {
  const wlr_output_image_description output = hdrOutputDescription();
  const umbriel::ColorDescription original = umbriel::outputColorDescription(&output, 203.0F);
  const umbriel::ColorDescription brighter = umbriel::outputColorDescription(&output, 500.0F);
  const umbriel::ColorDescription restored = umbriel::outputColorDescription(&output, 203.0F);

  CHECK_EQ(original.luminances.reference, 203.0F);
  CHECK_EQ(brighter.luminances.reference, 500.0F);
  CHECK(!umbriel::colorDescriptionsEqual(original, brighter));
  CHECK(umbriel::colorDescriptionsEqual(original, restored));

  for (const auto* description : {&original, &brighter, &restored}) {
    CHECK_EQ(description->data.tf_named, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
    CHECK_EQ(description->data.primaries_named, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
    CHECK_EQ(description->luminances.min, 0.005F);
    CHECK_EQ(description->luminances.max, 10000.0F);
    CHECK(description->data.has_mastering_display_primaries);
    CHECK_EQ(description->data.mastering_display_primaries.red.x, 0.68F);
    CHECK_EQ(description->data.mastering_display_primaries.green.y, 0.69F);
    CHECK_EQ(description->data.mastering_display_primaries.blue.x, 0.15F);
    CHECK_EQ(description->data.mastering_display_primaries.white.y, 0.329F);
    CHECK(description->data.has_mastering_luminance);
    CHECK_EQ(description->data.mastering_luminance.min, 0.01F);
    CHECK_EQ(description->data.mastering_luminance.max, 1000.0F);
    CHECK_EQ(description->data.max_cll, 1000U);
    CHECK_EQ(description->data.max_fall, 400U);
    CHECK_EQ(description->luminanceMultiplier, 1.0F);
    CHECK(!description->requiresHdrOutput);
  }
}

UMBRIEL_TEST(sdrFeedbackKeepsNominalWhiteAcrossConfiguredWhiteChanges) {
  const umbriel::ColorDescription original = umbriel::outputColorDescription(nullptr, 203.0F);
  const umbriel::ColorDescription changed = umbriel::outputColorDescription(nullptr, 500.0F);

  CHECK(umbriel::colorDescriptionsEqual(original, changed));
  CHECK_EQ(changed.data.tf_named, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
  CHECK_EQ(changed.data.primaries_named, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
  CHECK_EQ(changed.luminances.min, 0.2F);
  CHECK_EQ(changed.luminances.max, 80.0F);
  CHECK_EQ(changed.luminances.reference, 80.0F);
  CHECK(!changed.data.has_mastering_luminance);
  CHECK(!changed.data.has_mastering_display_primaries);
  CHECK_EQ(changed.data.max_cll, 0U);
  CHECK_EQ(changed.data.max_fall, 0U);
}

UMBRIEL_TEST(hdrWithoutDisplayMetadataStillUsesConfiguredWhite) {
  const wlr_output_image_description output{
      .primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020,
      .transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
      .mastering_display_primaries = {},
      .mastering_luminance = {},
      .max_cll = 0.0,
      .max_fall = 0.0,
  };
  const umbriel::ColorDescription description = umbriel::outputColorDescription(&output, 350.0F);

  CHECK_EQ(description.luminances.reference, 350.0F);
  CHECK_EQ(description.luminances.max, 10000.0F);
  CHECK(!description.data.has_mastering_luminance);
  CHECK(!description.data.has_mastering_display_primaries);
  CHECK_EQ(description.data.max_cll, 0U);
  CHECK_EQ(description.data.max_fall, 0U);
}

UMBRIEL_TEST(hdrToSdrDropsHdrTargetAndRestoresNominalWhite) {
  const wlr_output_image_description output = hdrOutputDescription();
  const umbriel::ColorDescription hdr = umbriel::outputColorDescription(&output, 500.0F);
  const umbriel::ColorDescription sdr = umbriel::outputColorDescription(nullptr, 500.0F);

  CHECK(!umbriel::colorDescriptionsEqual(hdr, sdr));
  CHECK_EQ(sdr.luminances.reference, 80.0F);
  CHECK(!sdr.data.has_mastering_display_primaries);
  CHECK(!sdr.data.has_mastering_luminance);
  CHECK_EQ(sdr.data.max_cll, 0U);
  CHECK_EQ(sdr.data.max_fall, 0U);
}

UMBRIEL_TEST(nonPqOutputKeepsTransferFunctionLuminanceDefaults) {
  const wlr_output_image_description output{
      .primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020,
      .transfer_function = WLR_COLOR_TRANSFER_FUNCTION_BT1886,
      .mastering_display_primaries = {},
      .mastering_luminance = {},
      .max_cll = 0.0,
      .max_fall = 0.0,
  };
  const umbriel::ColorDescription description = umbriel::outputColorDescription(&output, 500.0F);

  CHECK_EQ(description.data.tf_named, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886);
  CHECK_EQ(description.luminances.min, 0.01F);
  CHECK_EQ(description.luminances.max, 100.0F);
  CHECK_EQ(description.luminances.reference, 100.0F);
}

UMBRIEL_TEST(preferredDescriptionComparisonIncludesReferenceWhiteAndTargetMetadata) {
  const wlr_output_image_description output = hdrOutputDescription();
  const umbriel::ColorDescription original = umbriel::outputColorDescription(&output, 203.0F);
  std::array<umbriel::ColorDescription, 7> changed;
  changed.fill(original);
  changed[0].luminances.reference = 500.0F;
  changed[1].data.mastering_luminance.max = 800.0F;
  changed[2].data.mastering_display_primaries.red.x = 0.64F;
  changed[3].data.max_cll = 800;
  changed[4].data.max_fall = 300;
  changed[5].luminanceMultiplier = 80.0F / 203.0F;
  changed[6].requiresHdrOutput = true;

  for (const auto& description : changed) {
    CHECK(!umbriel::colorDescriptionsEqual(original, description));
  }
}

int main() { return RUN_TESTS(); }
