#pragma once

#include <string>

namespace umbriel {

  // Whether an output scans a client buffer out directly, which window owns it, or why the output composites instead.
  struct DirectScanoutState {
    bool active = false;
    // Why the output composites, as a snake_case name; empty while active.
    std::string reason = "no_candidate";
    // The scanned-out window; empty while inactive, or for a surface no window owns.
    std::string windowId;
    std::string appId;
    std::string title;

    bool operator==(const DirectScanoutState&) const = default;
  };

  // Scanout can change every frame, for instance while a cursor or a notification comes and goes over a fullscreen
  // game. Reports follow only a state that held for kSettleMs, so the log and the IPC stream see each lasting change
  // once.
  class DirectScanoutTracker {
  public:
    static constexpr int kSettleMs = 500;

    enum class Observation {
      // Same as the previous frame: a pending wait keeps running.
      Unchanged,
      // A new state that differs from the settled one: (re)start the wait.
      Pending,
      // Back to the settled state: drop a pending wait.
      Settled,
    };

    // Records the latest frame's state.
    Observation observe(const DirectScanoutState& state) {
      if (state == m_observed) {
        return Observation::Unchanged;
      }
      m_observed = state;
      return m_observed != m_settled ? Observation::Pending : Observation::Settled;
    }

    // Adopts the observed state once it has held. True when the settled state changed.
    bool settle() {
      if (m_observed == m_settled) {
        return false;
      }
      m_previous = m_settled;
      m_settled = m_observed;
      return true;
    }

    [[nodiscard]] const DirectScanoutState& settled() const { return m_settled; }

    // Whether the last settle is worth a log line: scanout started or stopped, or moved to another window. A change
    // between two reasons to composite is not.
    [[nodiscard]] bool settledChangeIsNotable() const {
      return m_previous.active != m_settled.active || (m_settled.active && m_previous.windowId != m_settled.windowId);
    }

  private:
    DirectScanoutState m_observed;
    DirectScanoutState m_settled;
    DirectScanoutState m_previous;
  };

} // namespace umbriel
