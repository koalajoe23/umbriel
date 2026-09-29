#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/addon.h>

#include "types/wlr_scene.h"
#include "umbrielfx/types/surface_pacing.h"

static void surface_pacing_addon_destroy(struct wlr_addon *addon) {
	struct umbrielfx_surface_pacing *pacing = wl_container_of(addon, pacing, addon);

	wl_signal_emit_mutable(&pacing->events.destroy, pacing);

	wl_list_remove(&pacing->output_destroy.link);
	wlr_addon_finish(&pacing->addon);
	free(pacing);
}

static const struct wlr_addon_interface surface_pacing_addon_impl = {
	.name = "umbrielfx_surface_pacing",
	.destroy = surface_pacing_addon_destroy,
};

// A wlr_scene_output going away doesn't necessarily re-run the surface's own
// outputs_update bookkeeping (that depends on the surface's real scene
// membership having changed too), so the addon watches its current output's
// destroy event directly to avoid outliving it.
static void handle_output_destroy(struct wl_listener *listener, void *data) {
	struct umbrielfx_surface_pacing *pacing =
		wl_container_of(listener, pacing, output_destroy);
	struct wlr_output *destroyed = pacing->output;

	struct wlr_surface_output *surface_output;
	wl_list_for_each(surface_output, &pacing->surface->current_outputs, link) {
		if (surface_output->output == destroyed) {
			// The output can no longer pace frames for this surface, whether
			// or not the surface ever gets an explicit leave for it.
			// Writing to this wlroots-owned entry is intentional and harmless
			// in either listener order: wlroots destroys it from its own
			// listener on the same output destroy signal. If that listener
			// ran first, the entry is already gone and this loop finds
			// nothing; if ours runs first, the flag only keeps
			// umbrielfx_surface_frame_pacing_output below from picking the
			// dying output, and the entry is freed right after.
			surface_output->suspended = true;
			break;
		}
	}

	umbrielfx_surface_pacing_set_output(pacing,
		umbrielfx_surface_frame_pacing_output(pacing->surface));
}

// Keeps output_destroy aimed at pacing->output. Safe to call whether or not a
// listener is currently registered.
static void surface_pacing_watch_output(struct umbrielfx_surface_pacing *pacing) {
	wl_list_remove(&pacing->output_destroy.link);

	if (pacing->output != NULL) {
		pacing->output_destroy.notify = handle_output_destroy;
		wl_signal_add(&pacing->output->events.destroy, &pacing->output_destroy);
	} else {
		wl_list_init(&pacing->output_destroy.link);
	}
}

struct umbrielfx_surface_pacing *umbrielfx_surface_pacing_find(struct wlr_surface *surface) {
	struct wlr_addon *addon = wlr_addon_find(&surface->addons,
		&surface_pacing_addon_impl, &surface_pacing_addon_impl);
	if (addon == NULL) {
		return NULL;
	}

	struct umbrielfx_surface_pacing *pacing = wl_container_of(addon, pacing, addon);
	return pacing;
}

void umbrielfx_surface_pacing_set_output(struct umbrielfx_surface_pacing *pacing,
		struct wlr_output *output) {
	if (pacing->output == output) {
		return;
	}

	pacing->output = output;
	surface_pacing_watch_output(pacing);
	wl_signal_emit_mutable(&pacing->events.output_change, NULL);
}

struct umbrielfx_surface_pacing *umbrielfx_surface_pacing_get(struct wlr_surface *surface) {
	struct umbrielfx_surface_pacing *pacing = umbrielfx_surface_pacing_find(surface);
	if (pacing != NULL) {
		return pacing;
	}

	pacing = calloc(1, sizeof(*pacing));
	if (pacing == NULL) {
		return NULL;
	}

	pacing->surface = surface;
	pacing->output = umbrielfx_surface_frame_pacing_output(surface);
	wl_list_init(&pacing->output_destroy.link);

	wl_signal_init(&pacing->events.output_change);
	wl_signal_init(&pacing->events.frame_done);
	wl_signal_init(&pacing->events.destroy);

	wlr_addon_init(&pacing->addon, &surface->addons,
		&surface_pacing_addon_impl, &surface_pacing_addon_impl);

	surface_pacing_watch_output(pacing);

	return pacing;
}
