#ifndef UMBRIELFX_TYPES_SURFACE_PACING_H
#define UMBRIELFX_TYPES_SURFACE_PACING_H

#include <time.h>
#include <wayland-server-core.h>
#include <wlr/util/addon.h>

struct wlr_surface;
struct wlr_output;

// A frame-done instant forwarded for the surface's current frame-pacing
// output. `when` points at the timespec passed to the scene's frame_done
// event; it is only valid for the duration of the signal emission.
struct umbrielfx_surface_pacing_frame_event {
	struct wlr_output *output;
	const struct timespec *when;
};

// Protocol-agnostic per-surface hook reporting the output the scene graph
// picks for this surface's frame callbacks (see
// umbrielfx_surface_frame_pacing_output), and forwarding frame-done instants
// for that output. Nothing here knows about any particular Wayland protocol;
// consumers (e.g. a fifo-v1/commit-timing-v1 pacer) listen to its events.
struct umbrielfx_surface_pacing {
	struct wlr_surface *surface;
	struct wlr_output *output; // current frame-pacing output, NULL while hidden

	struct {
		struct wl_signal output_change; // data: NULL; read ->output
		struct wl_signal frame_done;    // struct umbrielfx_surface_pacing_frame_event
		struct wl_signal destroy;       // emitted before free, when the surface is destroyed
	} events;

	struct wlr_addon addon;

	// Tracks pacing->output going away without a matching real scene
	// membership change (e.g. its wlr_scene_output being destroyed).
	struct wl_listener output_destroy;
};

// Returns the surface's pacing addon, creating it if it doesn't exist yet.
struct umbrielfx_surface_pacing *umbrielfx_surface_pacing_get(struct wlr_surface *surface);

#endif
