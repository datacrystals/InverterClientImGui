#pragma once

#include "telemetry_protocol.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>

// Mirrors device output to disk so external tools (e.g. an LLM debugging the
// inverter) can read what happened without holding the serial port.
//
//   <dir>/console-YYYY-MM-DD.log    - every device console line, appended live
//   <dir>/telemetry-YYYY-MM-DD.jsonl- latest value of every signal, ~10 Hz
//
// Files rotate at local midnight. Created on first use; logging failures are
// silent (never disturbs the GUI or the telemetry path).
class TelemetryLogger {
public:
    explicit TelemetryLogger(std::string dir);

    // Call once per GUI frame with the current snapshot. Internally
    // decimates: console lines are written as they appear, telemetry at ~10 Hz.
    void logFrame(const TelemetryState& st);

    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    const std::string& directory() const { return dir_; }
    void setDirectory(const std::string& dir);

    void setTelemetryPeriodMs(int ms) { telem_period_ms_ = ms; }

private:
    std::string currentDateStr() const;
    std::ofstream openForDate(const std::string& prefix, const std::string& ext,
                              std::string& open_date);

    void writeConsole(const TelemetryState& st);
    void writeTelemetry(const TelemetryState& st);

    std::string dir_;
    bool enabled_ = true;

    uint64_t last_console_seq_ = 0;
    std::string console_open_date_;
    std::ofstream console_file_;

    int telem_period_ms_ = 100; // 10 Hz
    std::chrono::steady_clock::time_point last_telem_write_{};
    std::string telem_open_date_;
    std::ofstream telem_file_;
};
