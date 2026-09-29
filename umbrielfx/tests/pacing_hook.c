// Exercise umbrielfx_surface_pacing against the real scene-surface listeners:
// a protocol-agnostic per-surface hook reporting frame-pacing output changes
// and forwarding frame-done instants. Fixture style copied from
// capture_pacing.c (headless backend, no renderer or running desktop
// required).
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>

#include "types/wlr_scene.h"
#include "umbrielfx/types/surface_pacing.h"
#include "umbrielfx/types/wlr_scene.h"

static int failures;
#define CHECK(condition)                                                                          \
	do {                                                                                           \
		if (!(condition)) {                                                                       \
			fprintf(stderr, "%d: %s\n", __LINE__, #condition);                                    \
			failures++;                                                                           \
		}                                                                                          \
	} while (0)

static void update(struct wlr_scene_surface *surface, struct wlr_scene_output *output) {
	struct wlr_scene_outputs_update_event event = {.active = &output, .size = output ? 1 : 0};
	wl_signal_emit_mutable(&surface->buffer->events.outputs_update, &event);
}

static void frame(struct wlr_scene_surface *surface, struct wlr_scene_output *output) {
	struct wlr_scene_frame_done_event event = {.output = output, .when = {1, 0}};
	wl_signal_emit_mutable(&surface->buffer->events.frame_done, &event);
}

// Hand-built surface: only the fields the scene-surface listeners and
// umbrielfx_surface_pacing touch. A wlroots bump can add more.
static void init_surface(struct wlr_surface *surface, struct wl_client *client) {
	*surface = (struct wlr_surface){0};
	surface->resource = wl_resource_create(client, &wl_surface_interface, 1, 0);
	assert(surface->resource);
	wl_list_init(&surface->current_outputs);
	wl_list_init(&surface->current.frame_callback_list);
	wl_signal_init(&surface->events.destroy);
	wl_signal_init(&surface->events.commit);
	wlr_addon_set_init(&surface->addons);
	pixman_region32_init(&surface->opaque_region);
}

static void fini_surface(struct wlr_surface *surface) {
	wl_signal_emit_mutable(&surface->events.destroy, surface);
	pixman_region32_fini(&surface->opaque_region);
	wlr_addon_set_finish(&surface->addons);
}

struct output_change_counter {
	struct wl_listener listener;
	int count;
};

static void handle_output_change(struct wl_listener *listener, void *data) {
	struct output_change_counter *counter = wl_container_of(listener, counter, listener);
	counter->count++;
}

struct frame_done_recorder {
	struct wl_listener listener;
	int count;
	struct wlr_output *last_output;
};

static void handle_frame_done(struct wl_listener *listener, void *data) {
	struct frame_done_recorder *recorder = wl_container_of(listener, recorder, listener);
	struct umbrielfx_surface_pacing_frame_event *event = data;
	recorder->count++;
	recorder->last_output = event->output;
}

int main(void) {
	struct wl_display *display = wl_display_create();
	assert(display);
	int sockets[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
	struct wl_client *client = wl_client_create(display, sockets[0]);
	assert(client);
	struct wlr_backend *backend = wlr_headless_backend_create(wl_display_get_event_loop(display));
	assert(backend);
	struct wlr_output *output_a = wlr_headless_add_output(backend, 800, 600);
	struct wlr_output *output_b = wlr_headless_add_output(backend, 800, 600);
	struct wlr_output *capture = wlr_headless_add_output(backend, 800, 600);
	assert(output_a && output_b && capture);
	output_a->refresh = 60000;
	output_b->refresh = 60000;
	capture->refresh = 0;

	struct wlr_scene *desktop = wlr_scene_create(), *mirror = wlr_scene_create();
	assert(desktop && mirror);
	struct wlr_scene_output *desktop_output_a = wlr_scene_output_create(desktop, output_a);
	struct wlr_scene_output *desktop_output_b = wlr_scene_output_create(desktop, output_b);
	struct wlr_scene_output *capture_output = wlr_scene_output_create(mirror, capture);
	assert(desktop_output_a && desktop_output_b && capture_output);

	// enter
	{
		struct wlr_surface surface;
		init_surface(&surface, client);
		struct wlr_scene_surface *scene_surface = wlr_scene_surface_create(&desktop->tree, &surface);
		assert(scene_surface);

		struct umbrielfx_surface_pacing *pacing = umbrielfx_surface_pacing_get(&surface);
		assert(pacing);
		CHECK(pacing->surface == &surface);
		CHECK(pacing->output == NULL);

		struct output_change_counter changes = {.listener = {.notify = handle_output_change}};
		wl_signal_add(&pacing->events.output_change, &changes.listener);

		update(scene_surface, desktop_output_a);
		CHECK(pacing->output == output_a);
		CHECK(changes.count == 1);

		fini_surface(&surface);
	}

	// hide
	{
		struct wlr_surface surface;
		init_surface(&surface, client);
		struct wlr_scene_surface *scene_surface = wlr_scene_surface_create(&desktop->tree, &surface);
		assert(scene_surface);

		struct umbrielfx_surface_pacing *pacing = umbrielfx_surface_pacing_get(&surface);
		struct output_change_counter changes = {.listener = {.notify = handle_output_change}};
		wl_signal_add(&pacing->events.output_change, &changes.listener);

		update(scene_surface, desktop_output_a);
		CHECK(pacing->output == output_a);
		CHECK(changes.count == 1);

		update(scene_surface, NULL);
		CHECK(pacing->output == NULL);
		CHECK(changes.count == 2);

		// Repeating the hide must not emit another output_change.
		update(scene_surface, NULL);
		CHECK(changes.count == 2);

		fini_surface(&surface);
	}

	// move
	{
		struct wlr_surface surface;
		init_surface(&surface, client);
		struct wlr_scene_surface *scene_surface = wlr_scene_surface_create(&desktop->tree, &surface);
		assert(scene_surface);

		struct umbrielfx_surface_pacing *pacing = umbrielfx_surface_pacing_get(&surface);

		update(scene_surface, desktop_output_a);
		CHECK(pacing->output == output_a);

		update(scene_surface, desktop_output_b);
		CHECK(pacing->output == output_b);

		fini_surface(&surface);
	}

	// frame_done_filter
	{
		struct wlr_surface surface;
		init_surface(&surface, client);
		struct wlr_scene_surface *scene_surface = wlr_scene_surface_create(&desktop->tree, &surface);
		assert(scene_surface);

		struct umbrielfx_surface_pacing *pacing = umbrielfx_surface_pacing_get(&surface);
		update(scene_surface, desktop_output_a);
		CHECK(pacing->output == output_a);

		struct frame_done_recorder recorder = {.listener = {.notify = handle_frame_done}};
		wl_signal_add(&pacing->events.frame_done, &recorder.listener);

		// Non-pacing output: no addon frame_done.
		frame(scene_surface, desktop_output_b);
		CHECK(recorder.count == 0);

		// Pacing output: fires once with that output.
		frame(scene_surface, desktop_output_a);
		CHECK(recorder.count == 1);
		CHECK(recorder.last_output == output_a);

		fini_surface(&surface);
	}

	// capture_scene
	{
		struct wlr_surface surface;
		init_surface(&surface, client);
		struct wlr_scene_surface *real = wlr_scene_surface_create(&desktop->tree, &surface);
		struct wlr_scene_surface *copy = wlr_scene_surface_create(&mirror->tree, &surface);
		assert(real && copy);

		struct umbrielfx_surface_pacing *pacing = umbrielfx_surface_pacing_get(&surface);
		struct output_change_counter changes = {.listener = {.notify = handle_output_change}};
		wl_signal_add(&pacing->events.output_change, &changes.listener);

		update(real, desktop_output_a);
		CHECK(pacing->output == output_a);
		CHECK(changes.count == 1);

		// Starting capture must not replace the desktop pacing output.
		update(copy, capture_output);
		CHECK(pacing->output == output_a);
		CHECK(changes.count == 1);
		CHECK(pacing->output == umbrielfx_surface_frame_pacing_output(&surface));

		fini_surface(&surface);
	}

	// output_destroyed
	{
		struct wlr_surface surface;
		init_surface(&surface, client);
		struct wlr_scene_surface *scene_surface = wlr_scene_surface_create(&desktop->tree, &surface);
		assert(scene_surface);

		struct umbrielfx_surface_pacing *pacing = umbrielfx_surface_pacing_get(&surface);
		struct output_change_counter changes = {.listener = {.notify = handle_output_change}};
		wl_signal_add(&pacing->events.output_change, &changes.listener);

		update(scene_surface, desktop_output_b);
		CHECK(pacing->output == output_b);
		CHECK(changes.count == 1);

		// desktop_output_b and output_b do not survive this call.
		wlr_output_destroy(output_b);
		CHECK(pacing->output == NULL);
		CHECK(changes.count == 2);

		fini_surface(&surface);
	}

	// no_addon
	{
		struct wlr_surface surface;
		init_surface(&surface, client);
		struct wlr_scene_surface *scene_surface = wlr_scene_surface_create(&desktop->tree, &surface);
		assert(scene_surface);

		// No umbrielfx_surface_pacing_get call: running both hooks must not
		// allocate an addon as a side effect.
		update(scene_surface, desktop_output_a);
		frame(scene_surface, desktop_output_a);
		CHECK(umbrielfx_surface_pacing_find(&surface) == NULL);

		fini_surface(&surface);
	}

	wlr_scene_node_destroy(&desktop->tree.node);
	wlr_scene_node_destroy(&mirror->tree.node);
	wlr_backend_destroy(backend);
	wl_display_destroy_clients(display);
	wl_display_destroy(display);
	close(sockets[1]);
	return failures ? 1 : 0;
}
