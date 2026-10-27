//
// RT64
//

#pragma once

#include <chrono>
#include <string>

#include "common/rt64_wcw_logic.h"
#include "common/rt64_wcw_perf.h"

// [wcw dd] In-game performance metrics to the local Datadog Agent (aki-roadmap#125). Off by default; WCW_DD_METRICS=1
// turns it on. Every WcwLogic::DdWindowSeconds a background thread sends the window's aki.game.* gauges (frame pacing,
// GPU ms, process memory, handles, VRAM) as DogStatsD over UDP to 127.0.0.1 (WCW_DD_PORT, default 8125), tagged with
// the settings (WcwLogic::DdContext). Events: session start and end, settings changed, venue changed, device lost.
// Nothing leaves the PC except through the owner's own Agent; with no Agent the packets are dropped.
//   WCW_DD_TAGS=key:value,...  extra tags (conversion id, owner); keep them low cardinality.
//   WCW_DD_GAME=<key>          the game tag, when the executable's name doesn't give it (WcwLogic::ddGameFromExe).
//   WCW_DD_ECHO=1              also print every packet to stderr.
// The aggregation and the line format are WcwLogic (unit tested); this is the sink. Every call is a no-op when off.
namespace RT64::WcwDd {
    bool enabled();
    using Clock = std::chrono::steady_clock;
    // Present thread: a Present call. generated: an in-between frame; repeated: the real frame shown again.
    void notePresent(Clock::time_point t, bool generated, bool repeated);
    // A WCW perf GPU timing row (render or present); the GPU timer runs while this is enabled (WcwPerf::timingEnabled).
    void noteGpuRow(const WcwPerf::GpuRow &row);
    // Present thread: the current settings, every present. A change (after the first seconds) sends a settings
    // event. The venue field is ignored; noteVenue sets it.
    void noteContext(const WcwLogic::DdContext &context);
    // The ray tracer's detected venue changed (empty: none).
    void noteVenue(const std::string &venue);
    // Sends an event at once, tagged with the current context. alertType: info, success, warning or error.
    void event(const std::string &title, const std::string &text, const char *alertType);
};
