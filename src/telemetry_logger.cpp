#include "telemetry_logger.h"
#include "json_escape.h"

#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

// Compact float formatting for the JSONL log (std::to_string always emits
// six decimals, which bloats the high-frequency log lines).
static std::string fmtFloat(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", (double)v);
    return buf;
}

TelemetryLogger::TelemetryLogger(std::string dir) : dir_(std::move(dir)) {}

void TelemetryLogger::setDirectory(const std::string& dir) {
    dir_ = dir;
    console_open_date_.clear();
    telem_open_date_.clear();
    console_file_.close();
    telem_file_.close();
}

std::string TelemetryLogger::currentDateStr() const {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

std::ofstream TelemetryLogger::openForDate(const std::string& prefix, const std::string& ext,
                                           std::string& open_date) {
    std::string date = currentDateStr();
    if (!open_date.empty() && open_date == date) {
        return {}; // caller keeps its current file
    }
    open_date = date;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    fs::path p = fs::path(dir_) / (prefix + "-" + date + ext);
    std::ofstream f(p, std::ios::app);
    return f;
}

void TelemetryLogger::logFrame(const TelemetryState& st) {
    if (!enabled_) return;
    writeConsole(st);
    writeTelemetry(st);
}

void TelemetryLogger::writeConsole(const TelemetryState& st) {
    // Find first line newer than what we have written. Seq numbers are
    // monotonic and the deque may drop old lines, so scan from the back.
    size_t start = st.console.size();
    while (start > 0 && st.console[start - 1].seq > last_console_seq_) --start;
    if (start == st.console.size()) return; // nothing new

    std::ofstream fresh = openForDate("console", ".log", console_open_date_);
    if (fresh.is_open()) console_file_ = std::move(fresh);
    if (!console_file_.is_open()) return;

    for (size_t i = start; i < st.console.size(); ++i) {
        const ConsoleLine& ln = st.console[i];
        console_file_ << ln.text;
        if (ln.text.empty() || ln.text.back() != '\n') console_file_ << '\n';
    }
    console_file_.flush();
    last_console_seq_ = st.console.back().seq;
}

void TelemetryLogger::writeTelemetry(const TelemetryState& st) {
    auto now = std::chrono::steady_clock::now();
    if (last_telem_write_.time_since_epoch().count() != 0 &&
        now - last_telem_write_ < std::chrono::milliseconds(telem_period_ms_)) {
        return;
    }
    last_telem_write_ = now;

    std::ofstream fresh = openForDate("telemetry", ".jsonl", telem_open_date_);
    if (fresh.is_open()) telem_file_ = std::move(fresh);
    if (!telem_file_.is_open()) return;

    using namespace std::chrono;
    double unix_s = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() / 1000.0;

    telem_file_ << "{\"t\":" << std::to_string(unix_s)
                << ",\"rx_hz\":" << fmtFloat(st.rx_hz) << ",\"signals\":{";
    bool first = true;
    for (const auto& kv : st.latest) {
        telem_file_ << (first ? "\"" : ",\"") << jsonEscape(kv.first)
                    << "\":" << fmtFloat(kv.second);
        first = false;
    }
    telem_file_ << "}";
    if (!st.latest_str.empty()) {
        telem_file_ << ",\"strings\":{";
        first = true;
        for (const auto& kv : st.latest_str) {
            telem_file_ << (first ? "\"" : ",\"") << jsonEscape(kv.first)
                        << "\":\"" << jsonEscape(kv.second) << "\"";
            first = false;
        }
        telem_file_ << "}";
    }
    telem_file_ << "}\n";
    telem_file_.flush();
}
