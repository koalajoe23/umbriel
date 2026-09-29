#ifndef TYPES_WLR_SCENE_H
#define TYPES_WLR_SCENE_H

#include <wlr/types/wlr_scene.h>

struct umbrielfx_surface_pacing;

struct wlr_scene *scene_node_get_root(struct wlr_scene_node *node);

void scene_node_get_size(struct wlr_scene_node *node, int *width, int *height);

void scene_surface_set_clip(struct wlr_scene_surface *surface, struct wlr_box *clip);

// Output used for frame pacing (surface frame callbacks, presentation time
// feedback, etc), may be NULL. Skips outputs suspended in the surface's own
// scene (see the capture/desktop note in umbrielfx/README.md).
struct wlr_output *umbrielfx_surface_frame_pacing_output(struct wlr_surface *surface);

// Finds the surface's pacing addon without creating one.
struct umbrielfx_surface_pacing *umbrielfx_surface_pacing_find(struct wlr_surface *surface);

// Sets the addon's current frame-pacing output, emitting events.output_change
// only when it actually changes.
void umbrielfx_surface_pacing_set_output(struct umbrielfx_surface_pacing *pacing,
	struct wlr_output *output);

#endif
