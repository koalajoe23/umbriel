#ifndef UMBRIELFX_TYPES_SCENE_CAPTURE_H
#define UMBRIELFX_TYPES_SCENE_CAPTURE_H

#include <wlr/types/wlr_ext_image_capture_source_v1.h>

/**
 * Set the render scale of a source created with
 * wlr_ext_image_capture_source_v1_create_with_scene_node().
 * The scale must be finite and positive. Active sessions receive updated buffer
 * constraints and a new frame; idle sources do not render until started.
 */
void umbrielfx_scene_capture_source_set_scale(struct wlr_ext_image_capture_source_v1* source, float scale);

#endif
