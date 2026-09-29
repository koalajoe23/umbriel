// Drives the frame-pacing protocols (fifo-v1, commit-timing-v1) from an xdg toplevel with shm buffers whose colour
// changes on every commit, and reports each commit's wp_presentation feedback. Log lines, one per event:
//   mapped                          the toplevel's first buffer is committed
//   refresh-mhz <n>                 a wl_output's current mode refresh rate
//   presented <index> <nsec>        commit <index> was presented at <nsec> (CLOCK_MONOTONIC)
//   discarded <index> <nsec>        commit <index> was never presented; <nsec> is when that was reported
//                                   (CLOCK_MONOTONIC)
//   target <index> <nsec>           commit <index> carries a commit-timing target of <nsec>
//   protocol-error <interface> <code>
//   done                            every commit of a burst has resolved its feedback
// Single-character commands on stdin start bursts; what each one does depends on the mode.
// Usage: pacing-client <mode> [args]. Modes:
//   map                      maps, logs "mapped", idles
//   fifo <count>             binds a wp_fifo_v1; on "b", <count> commits back to back, each with set_barrier and
//                            wait_barrier and no frame callback, then "done" once all their feedback resolved
//   fifo-duplicate           asks for two wp_fifo_v1 objects for the surface, which is a protocol error
//   fifo-destroy-mid <count> as fifo, but destroys the wp_fifo_v1 right after the burst and commits once more
//   timing <offset-ms>       binds a wp_commit_timer_v1; on "b", one commit targeted <offset-ms> from now, logging
//                            "target 0 <nsec>" before its feedback, then "done"
//   timing-fifo <offset-ms>  as timing, but also binds a wp_fifo_v1: on "b", a commit that sets the barrier, then the
//                            targeted commit, which also sets and waits for the barrier
//   timer-destroy-mid <offset-ms>
//                            as timing, but destroys the wp_commit_timer_v1 right after the timed commit
//   timing-invalid           sets a timestamp whose tv_nsec is out of range, which is a protocol error
//   timing-duplicate         sets two timestamps for one commit, which is a protocol error
//   timer-duplicate          asks for two wp_commit_timer_v1 objects for the surface, which is a protocol error
// Exits non-zero, after a message on stderr, when it cannot produce a commit it was asked for.

#include "commit-timing-v1-client-protocol.h"
#include "fifo-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <optional>
#include <poll.h>
#include <print>
#include <span>
#include <string_view>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>

namespace {

  // Buffers stay small whatever size the window is configured to: a burst keeps one buffer per held commit alive.
  constexpr int kBufferSize = 64;
  constexpr int kBufferStride = kBufferSize * 4;
  constexpr size_t kBufferBytes = static_cast<size_t>(kBufferStride) * kBufferSize;
  // Buffer slots beyond a burst's commit count: the mapping commit's buffer, a trailing commit, and slack for
  // releases still in flight.
  constexpr uint32_t kSpareSlots = 4;

  struct State;

  // What a mode does once the toplevel is mapped, and with each stdin command.
  struct Mode {
    std::string_view name;
    // Returns false when the arguments are unusable.
    bool (*parse)(State& state, std::span<char*> args);
    void (*mapped)(State& state);
    void (*command)(State& state, char command);
  };

  // One commit carrying presentation feedback, identified by its index in commit order.
  struct Feedback {
    State* state = nullptr;
    // Elaborated: the request function of the same name hides the type.
    struct wp_presentation_feedback* feedback = nullptr;
    uint32_t index = 0;
  };

  // One buffer in the client's shared-memory pool, reused once the compositor releases it.
  struct Slot {
    wl_buffer* buffer = nullptr;
    uint32_t* pixels = nullptr;
    bool busy = false;
  };

  struct State {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wmBase = nullptr;
    wp_presentation* presentation = nullptr;
    wp_fifo_manager_v1* fifoManager = nullptr;
    wp_commit_timing_manager_v1* commitTimingManager = nullptr;
    std::vector<wl_output*> outputs;
    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wp_fifo_v1* fifo = nullptr;
    wp_commit_timer_v1* timer = nullptr;
    const Mode* mode = nullptr;
    bool mapped = false;
    bool running = true;
    // Set when the client could not do what it was asked; main then exits non-zero.
    bool failed = false;
    // Commits per burst, from the mode's arguments.
    uint32_t burstCount = 0;
    // How far past the burst's start a timed commit targets, from the mode's arguments.
    uint64_t targetOffsetNsec = 0;
    // Buffer slots the pool is created with; a mode that holds many commits raises it while parsing.
    uint32_t slotCount = kSpareSlots;
    // Created once; each slot's address is its buffer's listener data, so the vector never grows afterwards.
    std::vector<Slot> slots;
    uint32_t nextSlot = 0;
    // Feedback commits so far; the next one gets this index.
    uint32_t nextIndex = 0;
    // Feedback objects still waiting for presented or discarded.
    uint32_t outstanding = 0;
    // Set by a mode while a burst runs; "done" is logged once its feedback has all resolved.
    bool burstActive = false;
  };

  void logLine(std::string_view line) {
    std::println("{}", line);
    std::fflush(stdout);
  }

  uint64_t monotonicNsec() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (static_cast<uint64_t>(now.tv_sec) * 1'000'000'000ULL) + static_cast<uint64_t>(now.tv_nsec);
  }

  // Resolving the last outstanding feedback of a burst ends it.
  void checkBurstDone(State& state) {
    if (state.burstActive && state.outstanding == 0) {
      state.burstActive = false;
      logLine("done");
    }
  }

  void fail(State& state, std::string_view message) {
    std::println(stderr, "pacing-client: {}", message);
    state.failed = true;
    state.running = false;
  }

  void bufferRelease(void* data, wl_buffer* /*buffer*/) { static_cast<Slot*>(data)->busy = false; }

  constexpr wl_buffer_listener kBufferListener = {
      .release = bufferRelease,
  };

  // Creates the shared-memory pool, one memfd split into state.slotCount buffers.
  bool createBuffers(State& state) {
    const size_t bytes = kBufferBytes * state.slotCount;
    const int fd = memfd_create("umbriel-pacing-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(bytes)) < 0) {
      if (fd >= 0) {
        close(fd);
      }
      return false;
    }
    void* pixels = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
      close(fd);
      return false;
    }
    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(bytes));
    close(fd);
    state.slots.resize(state.slotCount);
    for (uint32_t i = 0; i < state.slotCount; ++i) {
      Slot& slot = state.slots[i];
      const size_t offset = kBufferBytes * i;
      slot.pixels = static_cast<uint32_t*>(pixels) + (offset / sizeof(uint32_t));
      slot.buffer = wl_shm_pool_create_buffer(
          pool, static_cast<int>(offset), kBufferSize, kBufferSize, kBufferStride, WL_SHM_FORMAT_ARGB8888
      );
      wl_buffer_add_listener(slot.buffer, &kBufferListener, &slot);
    }
    // The buffers keep the pool's memory; the mapping lives as long as the client.
    wl_shm_pool_destroy(pool);
    return true;
  }

  // Attaches the next free buffer, filled with a colour that cycles with `seed` so consecutive commits always carry
  // different content the compositor has to present, and damages the whole surface, without committing.
  bool attachNewContent(State& state, uint32_t seed) {
    Slot* slot = nullptr;
    for (uint32_t tried = 0; tried < state.slots.size() && slot == nullptr; ++tried) {
      Slot& candidate = state.slots[state.nextSlot];
      state.nextSlot = (state.nextSlot + 1) % static_cast<uint32_t>(state.slots.size());
      if (!candidate.busy) {
        slot = &candidate;
      }
    }
    if (slot == nullptr) {
      fail(state, std::format("all {} buffers are still held by the compositor", state.slots.size()));
      return false;
    }
    const uint32_t channel = (seed * 37U) % 256U;
    const uint32_t color = 0xFF000000U | (channel << 16U) | ((255U - channel) << 8U) | ((seed * 91U) % 256U);
    std::fill_n(slot->pixels, kBufferBytes / sizeof(uint32_t), color);
    slot->busy = true;
    wl_surface_attach(state.surface, slot->buffer, 0, 0);
    wl_surface_damage_buffer(state.surface, 0, 0, kBufferSize, kBufferSize);
    return true;
  }

  void feedbackSyncOutput(void* /*data*/, struct wp_presentation_feedback* /*feedback*/, wl_output* /*output*/) {}

  void finishFeedback(Feedback* entry) {
    State& state = *entry->state;
    wp_presentation_feedback_destroy(entry->feedback);
    delete entry;
    --state.outstanding;
    checkBurstDone(state);
  }

  void feedbackPresented(
      void* data, struct wp_presentation_feedback* /*feedback*/, uint32_t tvSecHi, uint32_t tvSecLo, uint32_t tvNsec,
      uint32_t /*refresh*/, uint32_t /*seqHi*/, uint32_t /*seqLo*/, uint32_t /*flags*/
  ) {
    auto* entry = static_cast<Feedback*>(data);
    const uint64_t seconds = (static_cast<uint64_t>(tvSecHi) << 32U) | tvSecLo;
    logLine(std::format("presented {} {}", entry->index, (seconds * 1'000'000'000ULL) + tvNsec));
    finishFeedback(entry);
  }

  void feedbackDiscarded(void* data, struct wp_presentation_feedback* /*feedback*/) {
    auto* entry = static_cast<Feedback*>(data);
    logLine(std::format("discarded {} {}", entry->index, monotonicNsec()));
    finishFeedback(entry);
  }

  constexpr wp_presentation_feedback_listener kFeedbackListener = {
      .sync_output = feedbackSyncOutput,
      .presented = feedbackPresented,
      .discarded = feedbackDiscarded,
  };

  // Attaches new content, asks for presentation feedback, and returns the commit's index. The caller adds any
  // per-commit protocol state (fifo barriers, timestamps) and then commits with wl_surface_commit. Returns nothing,
  // having failed the client, when no buffer is free.
  std::optional<uint32_t> prepareFeedbackCommit(State& state) {
    if (!attachNewContent(state, state.nextIndex + 1)) {
      return std::nullopt;
    }
    const uint32_t index = state.nextIndex++;
    auto* entry = new Feedback{.state = &state, .feedback = nullptr, .index = index};
    entry->feedback = wp_presentation_feedback(state.presentation, state.surface);
    wp_presentation_feedback_add_listener(entry->feedback, &kFeedbackListener, entry);
    ++state.outstanding;
    return index;
  }

  // --- modes -------------------------------------------------------------------------------------------------------

  bool parseNoArgs(State& /*state*/, std::span<char*> args) { return args.empty(); }
  void mappedIdle(State& /*state*/) {}
  void commandIgnored(State& /*state*/, char /*command*/) {}

  // Reads a single positive commit count and sizes the buffer pool to hold that many commits at once.
  bool parseCount(State& state, std::span<char*> args) {
    if (args.size() != 1) {
      return false;
    }
    const std::string_view text = args[0];
    uint32_t count = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
    if (error != std::errc{} || end != text.data() + text.size() || count == 0) {
      return false;
    }
    state.burstCount = count;
    state.slotCount = count + kSpareSlots;
    return true;
  }

  void mappedFifo(State& state) {
    if (state.fifoManager == nullptr) {
      fail(state, "the compositor does not advertise wp_fifo_manager_v1");
      return;
    }
    state.fifo = wp_fifo_manager_v1_get_fifo(state.fifoManager, state.surface);
  }

  // Commits the burst: every commit sets a barrier and waits for the previous one's, with no frame callbacks.
  bool commitFifoBurst(State& state) {
    state.burstActive = true;
    for (uint32_t i = 0; i < state.burstCount; ++i) {
      if (!prepareFeedbackCommit(state)) {
        return false;
      }
      wp_fifo_v1_set_barrier(state.fifo);
      wp_fifo_v1_wait_barrier(state.fifo);
      wl_surface_commit(state.surface);
    }
    return true;
  }

  void commandFifo(State& state, char command) {
    if (command == 'b' && state.fifo != nullptr) {
      commitFifoBurst(state);
    }
  }

  void mappedFifoDuplicate(State& state) {
    if (state.fifoManager == nullptr) {
      fail(state, "the compositor does not advertise wp_fifo_manager_v1");
      return;
    }
    state.fifo = wp_fifo_manager_v1_get_fifo(state.fifoManager, state.surface);
    wp_fifo_manager_v1_get_fifo(state.fifoManager, state.surface);
  }

  void commandFifoDestroyMid(State& state, char command) {
    if (command != 'b' || state.fifo == nullptr || !commitFifoBurst(state)) {
      return;
    }
    wp_fifo_v1_destroy(state.fifo);
    state.fifo = nullptr;
    if (prepareFeedbackCommit(state)) {
      wl_surface_commit(state.surface);
    }
  }

  // Reads a single target offset in milliseconds.
  bool parseOffset(State& state, std::span<char*> args) {
    if (args.size() != 1) {
      return false;
    }
    const std::string_view text = args[0];
    uint64_t offsetMsec = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), offsetMsec);
    if (error != std::errc{} || end != text.data() + text.size()) {
      return false;
    }
    state.targetOffsetNsec = offsetMsec * 1'000'000ULL;
    return true;
  }

  // Asks for the surface's commit timer. False, having failed the client, when the global is missing.
  bool getTimer(State& state) {
    if (state.commitTimingManager == nullptr) {
      fail(state, "the compositor does not advertise wp_commit_timing_manager_v1");
      return false;
    }
    state.timer = wp_commit_timing_manager_v1_get_timer(state.commitTimingManager, state.surface);
    return true;
  }

  void setTimestamp(State& state, uint64_t nsec) {
    const uint64_t seconds = nsec / 1'000'000'000ULL;
    wp_commit_timer_v1_set_timestamp(
        state.timer, static_cast<uint32_t>(seconds >> 32U), static_cast<uint32_t>(seconds),
        static_cast<uint32_t>(nsec % 1'000'000'000ULL)
    );
  }

  void mappedTiming(State& state) { getTimer(state); }

  // Prepares the burst's single feedback commit and gives it a target the mode's offset from now; the caller adds any
  // other per-commit state and commits.
  bool prepareTimedCommit(State& state) {
    state.burstActive = true;
    const std::optional<uint32_t> index = prepareFeedbackCommit(state);
    if (!index) {
      return false;
    }
    const uint64_t target = monotonicNsec() + state.targetOffsetNsec;
    setTimestamp(state, target);
    logLine(std::format("target {} {}", *index, target));
    return true;
  }

  void commandTiming(State& state, char command) {
    if (command == 'b' && state.timer != nullptr && prepareTimedCommit(state)) {
      wl_surface_commit(state.surface);
    }
  }

  // Destroying the timer leaves the commit's timing constraint in force.
  void commandTimerDestroyMid(State& state, char command) {
    if (command != 'b' || state.timer == nullptr || !prepareTimedCommit(state)) {
      return;
    }
    wl_surface_commit(state.surface);
    wp_commit_timer_v1_destroy(state.timer);
    state.timer = nullptr;
  }

  void mappedTimingFifo(State& state) {
    if (state.fifoManager == nullptr) {
      fail(state, "the compositor does not advertise wp_fifo_manager_v1");
      return;
    }
    if (getTimer(state)) {
      state.fifo = wp_fifo_manager_v1_get_fifo(state.fifoManager, state.surface);
    }
  }

  // A commit sets the barrier, so the timed commit that follows is held by the fifo as well as by its timestamp.
  void commandTimingFifo(State& state, char command) {
    if (command != 'b' || state.timer == nullptr || state.fifo == nullptr) {
      return;
    }
    wp_fifo_v1_set_barrier(state.fifo);
    wl_surface_commit(state.surface);
    if (!prepareTimedCommit(state)) {
      return;
    }
    wp_fifo_v1_set_barrier(state.fifo);
    wp_fifo_v1_wait_barrier(state.fifo);
    wl_surface_commit(state.surface);
  }

  void mappedTimingInvalid(State& state) {
    if (getTimer(state)) {
      wp_commit_timer_v1_set_timestamp(state.timer, 0, 1, 1'000'000'000U);
      wl_surface_commit(state.surface);
    }
  }

  void mappedTimingDuplicate(State& state) {
    if (getTimer(state)) {
      const uint64_t target = monotonicNsec() + 1'000'000'000ULL;
      setTimestamp(state, target);
      setTimestamp(state, target);
      wl_surface_commit(state.surface);
    }
  }

  void mappedTimerDuplicate(State& state) {
    if (getTimer(state)) {
      wp_commit_timing_manager_v1_get_timer(state.commitTimingManager, state.surface);
    }
  }

  constexpr std::array kModes = {
      Mode{.name = "map", .parse = parseNoArgs, .mapped = mappedIdle, .command = commandIgnored},
      Mode{.name = "fifo", .parse = parseCount, .mapped = mappedFifo, .command = commandFifo},
      Mode{.name = "fifo-duplicate", .parse = parseNoArgs, .mapped = mappedFifoDuplicate, .command = commandIgnored},
      Mode{.name = "fifo-destroy-mid", .parse = parseCount, .mapped = mappedFifo, .command = commandFifoDestroyMid},
      Mode{.name = "timing", .parse = parseOffset, .mapped = mappedTiming, .command = commandTiming},
      Mode{.name = "timing-fifo", .parse = parseOffset, .mapped = mappedTimingFifo, .command = commandTimingFifo},
      Mode{
          .name = "timer-destroy-mid", .parse = parseOffset, .mapped = mappedTiming, .command = commandTimerDestroyMid
      },
      Mode{.name = "timing-invalid", .parse = parseNoArgs, .mapped = mappedTimingInvalid, .command = commandIgnored},
      Mode{
          .name = "timing-duplicate", .parse = parseNoArgs, .mapped = mappedTimingDuplicate, .command = commandIgnored
      },
      Mode{.name = "timer-duplicate", .parse = parseNoArgs, .mapped = mappedTimerDuplicate, .command = commandIgnored},
  };

  const Mode* findMode(std::string_view name) {
    const auto* found = std::ranges::find(kModes, name, &Mode::name);
    return found == kModes.end() ? nullptr : found;
  }

  // --- xdg shell ---------------------------------------------------------------------------------------------------

  void xdgSurfaceConfigure(void* data, xdg_surface* xdgSurface, uint32_t serial) {
    auto& state = *static_cast<State*>(data);
    xdg_surface_ack_configure(xdgSurface, serial);
    if (state.mapped) {
      // The buffer size is fixed, so a later configure only needs its ack applied.
      wl_surface_commit(state.surface);
      return;
    }
    if (!attachNewContent(state, 0)) {
      return;
    }
    wl_surface_commit(state.surface);
    state.mapped = true;
    logLine("mapped");
    state.mode->mapped(state);
  }

  constexpr xdg_surface_listener kXdgSurfaceListener = {
      .configure = xdgSurfaceConfigure,
  };

  void toplevelConfigure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
  void toplevelClose(void*, xdg_toplevel*) {}

  constexpr xdg_toplevel_listener kToplevelListener = {
      .configure = toplevelConfigure,
      .close = toplevelClose,
      .configure_bounds = nullptr,
      .wm_capabilities = nullptr,
  };

  void wmBasePing(void*, xdg_wm_base* wmBase, uint32_t serial) { xdg_wm_base_pong(wmBase, serial); }

  constexpr xdg_wm_base_listener kWmBaseListener = {
      .ping = wmBasePing,
  };

  // --- outputs -----------------------------------------------------------------------------------------------------

  void
  outputGeometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {}

  void outputMode(void* /*data*/, wl_output* /*output*/, uint32_t flags, int32_t, int32_t, int32_t refresh) {
    if ((flags & WL_OUTPUT_MODE_CURRENT) != 0) {
      logLine(std::format("refresh-mhz {}", refresh));
    }
  }

  void outputDone(void*, wl_output*) {}
  void outputScale(void*, wl_output*, int32_t) {}
  void outputName(void*, wl_output*, const char*) {}
  void outputDescription(void*, wl_output*, const char*) {}

  constexpr wl_output_listener kOutputListener = {
      .geometry = outputGeometry,
      .mode = outputMode,
      .done = outputDone,
      .scale = outputScale,
      .name = outputName,
      .description = outputDescription,
  };

  // --- registry ----------------------------------------------------------------------------------------------------

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    const std::string_view iface = interface;
    if (iface == wl_compositor_interface.name) {
      state.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (iface == wl_shm_interface.name) {
      state.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (iface == xdg_wm_base_interface.name) {
      state.wmBase = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
      xdg_wm_base_add_listener(state.wmBase, &kWmBaseListener, &state);
    } else if (iface == wp_presentation_interface.name) {
      state.presentation =
          static_cast<wp_presentation*>(wl_registry_bind(registry, name, &wp_presentation_interface, 1));
    } else if (iface == wp_fifo_manager_v1_interface.name) {
      state.fifoManager =
          static_cast<wp_fifo_manager_v1*>(wl_registry_bind(registry, name, &wp_fifo_manager_v1_interface, 1));
    } else if (iface == wp_commit_timing_manager_v1_interface.name) {
      state.commitTimingManager = static_cast<wp_commit_timing_manager_v1*>(
          wl_registry_bind(registry, name, &wp_commit_timing_manager_v1_interface, 1)
      );
    } else if (iface == wl_output_interface.name) {
      auto* output =
          static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 4U)));
      wl_output_add_listener(output, &kOutputListener, &state);
      state.outputs.push_back(output);
    }
  }

  void registryGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener = {
      .global = registryGlobal,
      .global_remove = registryGlobalRemove,
  };

  // --- event loop --------------------------------------------------------------------------------------------------

  // Logs the protocol error that broke the connection, if that is what happened.
  void reportConnectionError(State& state) {
    if (wl_display_get_error(state.display) != EPROTO) {
      return;
    }
    const wl_interface* interface = nullptr;
    uint32_t id = 0;
    const uint32_t code = wl_display_get_protocol_error(state.display, &interface, &id);
    logLine(std::format("protocol-error {} {}", interface != nullptr ? interface->name : "unknown", code));
  }

  // Reads what stdin has and hands each non-whitespace character to the mode. Returns false once stdin is closed.
  bool readCommands(State& state) {
    std::array<char, 64> buffer{};
    const ssize_t count = read(STDIN_FILENO, buffer.data(), buffer.size());
    if (count < 0) {
      return errno == EINTR || errno == EAGAIN;
    }
    if (count == 0) {
      return false;
    }
    for (const char command : std::span(buffer.data(), static_cast<size_t>(count))) {
      if (command != '\n' && command != ' ' && command != '\r') {
        state.mode->command(state, command);
      }
    }
    return true;
  }

  // Dispatches Wayland events and stdin commands until the connection fails. Returns false on a connection error.
  bool runLoop(State& state) {
    bool stdinOpen = true;
    while (state.running) {
      while (wl_display_prepare_read(state.display) != 0) {
        if (wl_display_dispatch_pending(state.display) < 0) {
          return false;
        }
      }
      // A burst can queue more than the socket takes at once; the rest goes out once it is writable again.
      bool flushPending = false;
      if (wl_display_flush(state.display) < 0) {
        if (errno != EAGAIN) {
          wl_display_cancel_read(state.display);
          return false;
        }
        flushPending = true;
      }

      const short displayEvents = flushPending ? static_cast<short>(POLLIN | POLLOUT) : static_cast<short>(POLLIN);
      std::array<pollfd, 2> fds = {{
          {.fd = wl_display_get_fd(state.display), .events = displayEvents, .revents = 0},
          {.fd = stdinOpen ? STDIN_FILENO : -1, .events = POLLIN, .revents = 0},
      }};
      if (poll(fds.data(), fds.size(), -1) < 0) {
        wl_display_cancel_read(state.display);
        if (errno == EINTR) {
          continue;
        }
        return false;
      }

      if ((fds[0].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
        if (wl_display_read_events(state.display) < 0) {
          return false;
        }
      } else {
        wl_display_cancel_read(state.display);
      }
      if (wl_display_dispatch_pending(state.display) < 0) {
        return false;
      }
      if (stdinOpen && (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        stdinOpen = readCommands(state);
      }
    }
    return true;
  }

} // namespace

int main(int argc, char** argv) {
  const std::span<char*> args(argv, static_cast<size_t>(argc));
  if (args.size() < 2) {
    std::println(stderr, "usage: pacing-client <mode> [args]");
    return EXIT_FAILURE;
  }

  State state;
  state.mode = findMode(args[1]);
  if (state.mode == nullptr || !state.mode->parse(state, args.subspan(2))) {
    std::println(stderr, "pacing-client: unknown mode or bad arguments: {}", args[1]);
    return EXIT_FAILURE;
  }

  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "pacing-client: cannot connect to WAYLAND_DISPLAY");
    return EXIT_FAILURE;
  }

  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(state.display);
  wl_display_roundtrip(state.display);

  if (state.compositor == nullptr || state.shm == nullptr || state.wmBase == nullptr || state.presentation == nullptr) {
    std::println(stderr, "pacing-client: compositor is missing a required Wayland global");
    return EXIT_FAILURE;
  }

  if (!createBuffers(state)) {
    std::println(stderr, "pacing-client: failed to allocate the shared-memory buffers");
    return EXIT_FAILURE;
  }

  state.surface = wl_compositor_create_surface(state.compositor);
  state.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.surface);
  xdg_surface_add_listener(state.xdgSurface, &kXdgSurfaceListener, &state);
  state.toplevel = xdg_surface_get_toplevel(state.xdgSurface);
  xdg_toplevel_add_listener(state.toplevel, &kToplevelListener, &state);
  xdg_toplevel_set_title(state.toplevel, "pacing-client");
  xdg_toplevel_set_app_id(state.toplevel, "pacing-client");
  wl_surface_commit(state.surface);

  const bool clean = runLoop(state);
  if (!clean) {
    reportConnectionError(state);
  }

  // Outstanding feedback, buffers, outputs, and the toplevel all go away with the connection.
  wl_display_disconnect(state.display);
  return clean && !state.failed ? EXIT_SUCCESS : EXIT_FAILURE;
}
