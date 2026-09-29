#include "pacing/surface_pacer.h"

#include "pacing/pacing_clock.h"
#include "wlr.h"

#include <cassert>
#include <ctime>
#include <new>

namespace umbriel {

  namespace {

    void detach(wl_listener& listener) {
      wl_list_remove(&listener.link);
      wl_list_init(&listener.link);
    }

  } // namespace

  // The surface's addon set owns the pacer through this entry.
  struct SurfacePacer::Addon {
    static void destroy(wlr_addon* found) {
      Addon* addon = nullptr;
      addon = wl_container_of(found, addon, addon);
      delete addon->pacer;
    }

    static constexpr wlr_addon_interface kInterface = {
        .name = "umbriel_surface_pacer",
        .destroy = destroy,
    };

    wlr_addon addon{};
    SurfacePacer* pacer = nullptr;
  };

  SurfacePacer& SurfacePacer::from(wlr_surface* surface) {
    if (wlr_addon* found = wlr_addon_find(&surface->addons, &Addon::kInterface, &Addon::kInterface)) {
      Addon* addon = nullptr;
      addon = wl_container_of(found, addon, addon);
      return *addon->pacer;
    }
    // Created before this pacer's own addon, so on surface destruction the (newer) pacer addon is torn down first.
    umbrielfx_surface_pacing* pacing = umbrielfx_surface_pacing_get(surface);
    if (pacing == nullptr) {
      throw std::bad_alloc();
    }
    // Owned by the surface's addon set; freed by Addon::destroy.
    auto* pacer = new SurfacePacer(surface, pacing);
    return *pacer;
  }

  SurfacePacer::SurfacePacer(wlr_surface* surface, umbrielfx_surface_pacing* pacing)
      : m_addon(std::make_unique<Addon>()), m_pacing(pacing) {
    wl_signal_init(&events.refresh);
    wl_signal_init(&events.destroy);
    wl_list_init(&m_outputPresent.link);
    wl_list_init(&m_outputDestroy.link);

    m_addon->pacer = this;
    wlr_addon_init(&m_addon->addon, &surface->addons, &Addon::kInterface, &Addon::kInterface);

    m_outputChange.notify = onOutputChange;
    wl_signal_add(&pacing->events.output_change, &m_outputChange);
    m_frameDone.notify = onFrameDone;
    wl_signal_add(&pacing->events.frame_done, &m_frameDone);
    m_pacingDestroy.notify = onPacingDestroy;
    wl_signal_add(&pacing->events.destroy, &m_pacingDestroy);

    wl_display* display = wl_client_get_display(wl_resource_get_client(surface->resource));
    m_hiddenTick = wl_event_loop_add_timer(wl_display_get_event_loop(display), onHiddenTick, this);

    setOutput(pacing->output);
  }

  SurfacePacer::~SurfacePacer() {
    wl_signal_emit_mutable(&events.destroy, this);

    wl_list_remove(&m_outputChange.link);
    wl_list_remove(&m_frameDone.link);
    wl_list_remove(&m_pacingDestroy.link);
    wl_list_remove(&m_outputPresent.link);
    wl_list_remove(&m_outputDestroy.link);
    if (m_hiddenTick != nullptr) {
      wl_event_source_remove(m_hiddenTick);
    }
    wlr_addon_finish(&m_addon->addon);
  }

  Nanoseconds SurfacePacer::period() const {
    const int32_t modeRefreshMhz = m_output != nullptr ? m_output->refresh : m_lastOutputRefreshMhz;
    return refreshPeriod(m_lastPresentRefresh, modeRefreshMhz);
  }

  Nanoseconds SurfacePacer::lastKnownPeriod() const { return m_hadOutput ? period() : 0; }

  Nanoseconds SurfacePacer::predictFollowingPresent(Nanoseconds now) const {
    return umbriel::predictFollowingPresent(m_lastPresent, period(), now);
  }

  Nanoseconds SurfacePacer::predictReleaseFramePresent(Nanoseconds now) const {
    return umbriel::predictReleaseFramePresent(m_lastPresent, period(), now);
  }

  void SurfacePacer::requestFrame() {
    if (m_output != nullptr) {
      wlr_output_schedule_frame(m_output);
    }
  }

  void SurfacePacer::retain() {
    ++m_retainCount;
    updateHiddenTick();
  }

  void SurfacePacer::release() {
    assert(m_retainCount > 0);
    if (m_retainCount > 0) {
      --m_retainCount;
    }
    updateHiddenTick();
  }

  void SurfacePacer::onOutputChange(wl_listener* listener, void* /*data*/) {
    SurfacePacer* self = nullptr;
    self = wl_container_of(listener, self, m_outputChange);
    self->handleOutputChange();
  }

  void SurfacePacer::onFrameDone(wl_listener* listener, void* data) {
    SurfacePacer* self = nullptr;
    self = wl_container_of(listener, self, m_frameDone);
    self->handleFrameDone(data);
  }

  void SurfacePacer::onPacingDestroy(wl_listener* listener, void* /*data*/) {
    SurfacePacer* self = nullptr;
    self = wl_container_of(listener, self, m_pacingDestroy);
    // The pacer cannot outlive the hook it mirrors; normally its own addon goes first and this never runs.
    delete self;
  }

  void SurfacePacer::onOutputPresent(wl_listener* listener, void* data) {
    SurfacePacer* self = nullptr;
    self = wl_container_of(listener, self, m_outputPresent);
    self->handleOutputPresent(data);
  }

  void SurfacePacer::onOutputDestroy(wl_listener* listener, void* /*data*/) {
    SurfacePacer* self = nullptr;
    self = wl_container_of(listener, self, m_outputDestroy);
    self->handleOutputDestroy();
  }

  int SurfacePacer::onHiddenTick(void* data) {
    static_cast<SurfacePacer*>(data)->handleHiddenTick();
    return 0;
  }

  void SurfacePacer::handleOutputChange() { setOutput(m_pacing->output); }

  void SurfacePacer::handleFrameDone(void* data) {
    const auto* event = static_cast<const umbrielfx_surface_pacing_frame_event*>(data);
    if (m_output == nullptr || event->output != m_output) {
      return;
    }
    PacerRefreshEvent refresh{.when = toNanoseconds(*event->when), .hidden = false};
    wl_signal_emit_mutable(&events.refresh, &refresh);
  }

  void SurfacePacer::handleOutputPresent(void* data) {
    const auto* event = static_cast<const wlr_output_event_present*>(data);
    if (!event->presented) {
      return;
    }
    m_lastPresent = toNanoseconds(event->when);
    m_lastPresentRefresh = event->refresh;
  }

  // Hidden until umbrielfx reports the surface's next pacing output.
  void SurfacePacer::handleOutputDestroy() { setOutput(nullptr); }

  void SurfacePacer::handleHiddenTick() {
    m_hiddenTickArmed = false;
    if (m_output != nullptr || m_retainCount == 0) {
      return;
    }
    // Re-armed before emitting so a listener that releases the pacer disarms the next tick.
    armHiddenTick();
    PacerRefreshEvent refresh{.when = monotonicNow(), .hidden = true};
    wl_signal_emit_mutable(&events.refresh, &refresh);
  }

  void SurfacePacer::setOutput(wlr_output* output) {
    if (output == m_output) {
      return;
    }
    if (m_output != nullptr) {
      m_lastOutputRefreshMhz = m_output->refresh;
    }
    detach(m_outputPresent);
    detach(m_outputDestroy);

    m_output = output;
    if (output != nullptr) {
      // The previous present belonged to another output, or to this one before it hid the surface.
      m_lastPresent = 0;
      m_lastPresentRefresh = 0;
      m_lastOutputRefreshMhz = output->refresh;
      m_hadOutput = true;
      m_outputPresent.notify = onOutputPresent;
      wl_signal_add(&output->events.present, &m_outputPresent);
      m_outputDestroy.notify = onOutputDestroy;
      wl_signal_add(&output->events.destroy, &m_outputDestroy);
    }
    updateHiddenTick();
  }

  void SurfacePacer::updateHiddenTick() {
    if (m_output == nullptr && m_retainCount > 0) {
      if (!m_hiddenTickArmed) {
        armHiddenTick();
      }
      return;
    }
    if (m_hiddenTickArmed && m_hiddenTick != nullptr) {
      wl_event_source_timer_update(m_hiddenTick, 0);
    }
    m_hiddenTickArmed = false;
  }

  void SurfacePacer::armHiddenTick() {
    if (m_hiddenTick == nullptr) {
      return;
    }
    wl_event_source_timer_update(m_hiddenTick, timerDelayMsecNearest(hiddenTickInterval(lastKnownPeriod())));
    m_hiddenTickArmed = true;
  }

} // namespace umbriel
