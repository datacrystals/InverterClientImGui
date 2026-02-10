// telemetry_imgui_client.cpp
// Changes requested:
// - Remove console reading of stdio completely (no RX->console parsing)
// - Console only logs commands typed/sent + send status
// - Main window title set to "RTE"
// - Add X/Y axis labeling for graphs (ImGui::PlotLines has no native axis labels)
//     -> Implement a lightweight custom plot widget using ImDrawList,
//        with labeled X (time, seconds) and Y (value) axes.
//
// Notes:
// - Telemetry binary parsing stays the same (COBS frames delimited by 0x00).
// - Graph uses actual time (seconds) from stored history for x-axis.
// - Pinned signals open in their own windows as before.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <limits>
#include <cmath>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <unistd.h>
  #include <termios.h>
  #include <sys/ioctl.h>
#endif

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

// ------------------ Telemetry structs (must match Pico) ------------------
#pragma pack(push, 1)
struct TelemetryHeader {
    uint32_t magic;       // "TLM1" = 0x544C4D31
    uint8_t  version;     // 1
    uint8_t  msg_type;    // 1 = telemetry
    uint16_t payload_len; // bytes
    uint32_t seq;
    uint32_t time_us;     // time_us_32()
};

struct TelemetryPayloadV1 {
    float v_dc, v_u, v_v, v_w;
    float i_dc_main, i_u, i_w;
    float enc_sin, enc_cos, rotor_deg;
    float sensor_rate_khz;
};
#pragma pack(pop)

static constexpr uint32_t MAGIC = 0x544C4D31u; // "TLM1"
static constexpr uint8_t  VERSION = 1;
static constexpr uint8_t  MSG_TELEMETRY = 1;

// ------------------ CRC16-CCITT ------------------
static uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// ------------------ COBS decode ------------------
static size_t cobs_decode(const uint8_t* in, size_t len, uint8_t* out, size_t out_cap) {
    size_t r = 0, w = 0;
    while (r < len) {
        uint8_t code = in[r++];
        if (code == 0) throw std::runtime_error("COBS: zero code");
        for (uint8_t i = 1; i < code; ++i) {
            if (r >= len) throw std::runtime_error("COBS: overrun");
            if (w >= out_cap) throw std::runtime_error("COBS: out_cap");
            out[w++] = in[r++];
        }
        if (code != 0xFF && r < len) {
            if (w >= out_cap) throw std::runtime_error("COBS: out_cap");
            out[w++] = 0;
        }
    }
    return w;
}

// ------------------ Serial abstraction ------------------
#ifdef _WIN32
using SerialHandle = HANDLE;
static constexpr SerialHandle INVALID_SERIAL = INVALID_HANDLE_VALUE;
#else
using SerialHandle = int;
static constexpr SerialHandle INVALID_SERIAL = -1;
#endif

static std::mutex g_serial_mtx;
static SerialHandle g_serial = INVALID_SERIAL;

#ifdef _WIN32
static SerialHandle serial_open(const char* port_name) {
    std::string full = "\\\\.\\" + std::string(port_name);
    HANDLE h = CreateFileA(full.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("CreateFile failed");

    DCB dcb{}; dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) throw std::runtime_error("GetCommState failed");
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(h, &dcb)) throw std::runtime_error("SetCommState failed");

    COMMTIMEOUTS t{};
    t.ReadIntervalTimeout = 1;
    t.ReadTotalTimeoutConstant = 1;
    SetCommTimeouts(h, &t);

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return h;
}
static void serial_close(SerialHandle h) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
static int serial_read(SerialHandle h, uint8_t* buf, int cap) {
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)cap, &got, nullptr)) return 0;
    return (int)got;
}
static bool serial_write(SerialHandle h, const uint8_t* data, int n) {
    DWORD wrote = 0;
    if (!WriteFile(h, data, (DWORD)n, &wrote, nullptr)) return false;
    return (int)wrote == n;
}
#else
static SerialHandle serial_open(const char* dev) {
    int fd = ::open(dev, O_RDWR | O_NOCTTY);
    if (fd < 0) throw std::runtime_error("open() failed");

    termios tty{};
    if (tcgetattr(fd, &tty) != 0) throw std::runtime_error("tcgetattr failed");
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    cfmakeraw(&tty);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cc[VMIN] = 1; tty.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tty) != 0) throw std::runtime_error("tcsetattr failed");

    int flags = 0;
    if (ioctl(fd, TIOCMGET, &flags) == -1) throw std::runtime_error("TIOCMGET failed");
    flags |= (TIOCM_DTR | TIOCM_RTS);
    if (ioctl(fd, TIOCMSET, &flags) == -1) throw std::runtime_error("TIOCMSET failed");

    tcflush(fd, TCIFLUSH);
    return fd;
}
static void serial_close(SerialHandle fd) { if (fd >= 0) ::close(fd); }
static int serial_read(SerialHandle fd, uint8_t* buf, int cap) {
    int n = (int)::read(fd, buf, cap);
    return n > 0 ? n : 0;
}
static bool serial_write(SerialHandle fd, const uint8_t* data, int n) {
    int wrote = (int)::write(fd, data, (size_t)n);
    return wrote == n;
}
#endif

static bool send_text_line(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_serial_mtx);
    if (g_serial == INVALID_SERIAL) return false;
    std::string out = s;
    if (out.empty() || (out.back() != '\n' && out.back() != '\r')) out.push_back('\n');
    return serial_write(g_serial, (const uint8_t*)out.data(), (int)out.size());
}

// ------------------ Telemetry storage ------------------
struct SignalHistory { std::deque<float> y, t; };

struct TelemetryState {
    std::unordered_map<std::string, float> latest;
    std::unordered_map<std::string, SignalHistory> hist;
    uint32_t last_seq = 0;
    float rx_hz = 0.0f;
    uint64_t good_frames = 0, bad_frames = 0;
};

static std::mutex g_state_mtx;
static TelemetryState g_state;
static std::atomic<bool> g_run{true};

static std::atomic<uint64_t> g_reject_decode{0}, g_reject_crc{0}, g_reject_hdr{0}, g_reject_len{0};

static float g_retain_seconds = 30.0f;
static int   g_max_samples_cap = 12000;

static inline void trim_history(SignalHistory& H) {
    if (!H.t.empty()) {
        float newest = H.t.back();
        float oldest_allowed = newest - g_retain_seconds;
        while (!H.t.empty() && H.t.front() < oldest_allowed) {
            H.t.pop_front(); H.y.pop_front();
        }
    }
    while ((int)H.t.size() > g_max_samples_cap) { H.t.pop_front(); H.y.pop_front(); }
}

static void ingest_payload_locked(const TelemetryHeader& h, const TelemetryPayloadV1& p, float tsec) {
    auto& S = g_state;
    const auto set = [&](const char* name, float v) {
        S.latest[name] = v;
        auto& H = S.hist[name];
        H.t.push_back(tsec);
        H.y.push_back(v);
        trim_history(H);
    };

    set("V_DC_BUS", p.v_dc);
    set("V_PH_U",   p.v_u);
    set("V_PH_V",   p.v_v);
    set("V_PH_W",   p.v_w);
    set("I_DC_MAIN", p.i_dc_main);
    set("I_PH_U",    p.i_u);
    set("I_PH_W",    p.i_w);
    set("ENCODER_SIN", p.enc_sin);
    set("ENCODER_COS", p.enc_cos);
    set("ROTOR_DEG",   p.rotor_deg);
    set("SENSOR_RATE_KHZ", p.sensor_rate_khz);

    S.last_seq = h.seq;
    S.good_frames++;
}

// ------------------ Serial thread (telemetry only) ------------------
static void serial_thread_fn(const std::string& port) {
    try {
        SerialHandle h = serial_open(port.c_str());
        {
            std::lock_guard<std::mutex> lk(g_serial_mtx);
            g_serial = h;
        }

        std::vector<uint8_t> frame;
        frame.reserve(256);

        auto t0 = std::chrono::steady_clock::now();
        uint64_t frames_in_window = 0;
        auto hz_window_start = t0;

        uint8_t buf[512];

        while (g_run.load()) {
            int n = serial_read(h, buf, (int)sizeof(buf));
            if (n == 0) continue;

            for (int i = 0; i < n; ++i) {
                uint8_t b = buf[i];
                if (b == 0x00) {
                    if (!frame.empty()) {
                        try {
                            uint8_t decoded[256];
                            size_t dec_len = 0;
                            try { dec_len = cobs_decode(frame.data(), frame.size(), decoded, sizeof(decoded)); }
                            catch (...) { g_reject_decode++; throw; }

                            if (dec_len < sizeof(TelemetryHeader) + 2) { g_reject_len++; throw std::runtime_error("short"); }

                            uint16_t rx_crc = (uint16_t)decoded[dec_len - 2] | ((uint16_t)decoded[dec_len - 1] << 8);
                            uint16_t calc   = crc16_ccitt(decoded, dec_len - 2);
                            if (rx_crc != calc) { g_reject_crc++; throw std::runtime_error("crc"); }

                            TelemetryHeader thdr{};
                            std::memcpy(&thdr, decoded, sizeof(thdr));
                            if (thdr.magic != MAGIC || thdr.version != VERSION || thdr.msg_type != MSG_TELEMETRY) {
                                g_reject_hdr++; throw std::runtime_error("hdr");
                            }

                            if (thdr.payload_len != sizeof(TelemetryPayloadV1) ||
                                sizeof(TelemetryHeader) + thdr.payload_len + 2 != dec_len) {
                                g_reject_len++; throw std::runtime_error("len");
                            }

                            TelemetryPayloadV1 p{};
                            std::memcpy(&p, decoded + sizeof(TelemetryHeader), sizeof(p));

                            auto now = std::chrono::steady_clock::now();
                            float tsec = std::chrono::duration<float>(now - t0).count();

                            {
                                std::lock_guard<std::mutex> lk(g_state_mtx);
                                ingest_payload_locked(thdr, p, tsec);
                            }

                            frames_in_window++;
                            float dt = std::chrono::duration<float>(now - hz_window_start).count();
                            if (dt >= 1.0f) {
                                float hz = frames_in_window / dt;
                                {
                                    std::lock_guard<std::mutex> lk(g_state_mtx);
                                    g_state.rx_hz = hz;
                                }
                                frames_in_window = 0;
                                hz_window_start = now;
                            }
                        } catch (...) {
                            std::lock_guard<std::mutex> lk(g_state_mtx);
                            g_state.bad_frames++;
                        }
                        frame.clear();
                    }
                } else {
                    if (frame.size() < 255) frame.push_back(b);
                    else frame.clear();
                }
            }
        }

        {
            std::lock_guard<std::mutex> lk(g_serial_mtx);
            g_serial = INVALID_SERIAL;
        }
        serial_close(h);
    } catch (const std::exception& e) {
        std::cerr << "Serial thread fatal: " << e.what() << "\n";
        std::lock_guard<std::mutex> lk(g_serial_mtx);
        g_serial = INVALID_SERIAL;
    }
}

// ------------------ Command log console (LOCAL ONLY) ------------------
static std::mutex g_cmdlog_mtx;
static std::deque<std::string> g_cmdlog;
static size_t g_cmdlog_cap = 200;

static void cmdlog_push(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_cmdlog_mtx);
    g_cmdlog.push_back(s);
    while (g_cmdlog.size() > g_cmdlog_cap) g_cmdlog.pop_front();
}

// ------------------ Pins/plots ------------------
struct PinnedWindow {
    std::string name;
    bool open = true;
    float view_seconds = 10.0f;
    bool autoscale = true;
    float manual_min = -1.0f;
    float manual_max =  1.0f;

    char y_label[64] = "Value";
};

static std::vector<PinnedWindow> g_pins;

static int find_pin_index(const std::string& name) {
    for (int i = 0; i < (int)g_pins.size(); ++i) if (g_pins[i].name == name) return i;
    return -1;
}
static void pin_signal(const std::string& name) {
    if (find_pin_index(name) >= 0) return;

    PinnedWindow w;
    w.name = name;

    auto set_label = [&](const char* s) {
        std::snprintf(w.y_label, sizeof(w.y_label), "%s", s);
    };

    if (name.rfind("V_", 0) == 0) set_label("Volts (V)");
    else if (name.rfind("I_", 0) == 0) set_label("Amps (A)");
    else if (name.find("ROTOR") != std::string::npos) set_label("Degrees (deg)");
    else if (name.find("RATE") != std::string::npos) set_label("kHz");
    else set_label("Value");

    g_pins.push_back(w);
}

static void remove_pin_at(int idx) {
    if (idx < 0 || idx >= (int)g_pins.size()) return;
    g_pins.erase(g_pins.begin() + idx);
}

static void compute_minmax(const float* data, int n, float& out_min, float& out_max) {
    float mn = std::numeric_limits<float>::infinity();
    float mx = -std::numeric_limits<float>::infinity();
    for (int i = 0; i < n; ++i) {
        float v = data[i];
        if (!std::isfinite(v)) continue;
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    if (!std::isfinite(mn) || !std::isfinite(mx)) { mn = -1.0f; mx = 1.0f; }
    if (mn == mx) { mn -= 1.0f; mx += 1.0f; }
    else { float pad = (mx - mn) * 0.08f; mn -= pad; mx += pad; }
    out_min = mn; out_max = mx;
}

static void format_float(char* out, size_t out_sz, float v) {
    std::snprintf(out, out_sz, "%g", (double)v);
}

// Custom simple line plot with axis labels.
// Draw area is provided by ImGui::InvisibleButton.
// X axis: time (seconds) based on t array.
// Y axis: value.
// Uses ImDrawList only.
static void DrawLabeledTimePlot(const char* id,
                               const float* t, const float* y, int n,
                               float t_min, float t_max,
                               float y_min, float y_max,
                               const char* x_label,
                               const char* y_label,
                               ImVec2 size)
{
    if (n <= 1) {
        ImGui::TextDisabled("Not enough samples");
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();

    if (size.x <= 0) size.x = ImGui::GetContentRegionAvail().x;
    if (size.y <= 0) size.y = 260.0f;

    // Layout: reserve margins for labels/ticks
    const float left = 58.0f;
    const float right = 12.0f;
    const float top = 12.0f;
    const float bottom = 40.0f;

    ImVec2 plot_min = ImVec2(p0.x + left, p0.y + top);
    ImVec2 plot_max = ImVec2(p0.x + size.x - right, p0.y + size.y - bottom);

    ImGui::InvisibleButton(id, size);

    // Background
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(20,20,20,255), 6.0f);
    dl->AddRect(plot_min, plot_max, IM_COL32(120,120,120,255), 0.0f);

    auto xmap = [&](float tv) {
        float u = (tv - t_min) / (t_max - t_min);
        u = (u < 0) ? 0 : (u > 1 ? 1 : u);
        return plot_min.x + u * (plot_max.x - plot_min.x);
    };
    auto ymap = [&](float yv) {
        float u = (yv - y_min) / (y_max - y_min);
        u = (u < 0) ? 0 : (u > 1 ? 1 : u);
        return plot_max.y - u * (plot_max.y - plot_min.y);
    };

    // Grid + ticks
    const int grid_y = 4;
    const int grid_x = 5;

    for (int i = 0; i <= grid_y; ++i) {
        float a = (float)i / (float)grid_y;
        float yline = plot_min.y + a * (plot_max.y - plot_min.y);
        dl->AddLine(ImVec2(plot_min.x, yline), ImVec2(plot_max.x, yline), IM_COL32(60,60,60,255));

        float val = y_max - a * (y_max - y_min);
        char buf[32]; format_float(buf, sizeof(buf), val);
        dl->AddText(ImVec2(p0.x + 6.0f, yline - 7.0f), IM_COL32(200,200,200,255), buf);
    }

    for (int i = 0; i <= grid_x; ++i) {
        float a = (float)i / (float)grid_x;
        float xline = plot_min.x + a * (plot_max.x - plot_min.x);
        dl->AddLine(ImVec2(xline, plot_min.y), ImVec2(xline, plot_max.y), IM_COL32(60,60,60,255));

        float val = t_min + a * (t_max - t_min);
        char buf[32]; format_float(buf, sizeof(buf), val);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(xline - ts.x * 0.5f, plot_max.y + 6.0f), IM_COL32(200,200,200,255), buf);
    }

    // Axis labels
    // Y label (left side)
    dl->AddText(ImVec2(p0.x + 6.0f, p0.y + 6.0f), IM_COL32(220,220,220,255), y_label);

    // X label (bottom center)
    ImVec2 xls = ImGui::CalcTextSize(x_label);
    dl->AddText(ImVec2((plot_min.x + plot_max.x) * 0.5f - xls.x * 0.5f, p0.y + size.y - 18.0f),
                IM_COL32(220,220,220,255), x_label);

    // Polyline
    // (ImGui doesn't support thick antialiased polylines easily without special flags;
    // this is good enough for a telemetry viewer.)
    ImU32 col = IM_COL32(120, 220, 180, 255);
    ImVec2 prev = ImVec2(xmap(t[0]), ymap(y[0]));
    for (int i = 1; i < n; ++i) {
        ImVec2 cur = ImVec2(xmap(t[i]), ymap(y[i]));
        dl->AddLine(prev, cur, col, 2.0f);
        prev = cur;
    }
}

static void draw_signal_window(PinnedWindow& W, const TelemetryState& snap) {
    std::string title = W.name + "###PIN_" + W.name;
    if (!W.open) return;

    if (!ImGui::Begin(title.c_str(), &W.open)) { ImGui::End(); return; }

    auto itV = snap.latest.find(W.name);
    auto itH = snap.hist.find(W.name);
    float latest = (itV != snap.latest.end()) ? itV->second : 0.0f;

    ImGui::Text("Latest: % .6f", latest);
    ImGui::Separator();

    if (itH == snap.hist.end() || itH->second.y.empty()) {
        ImGui::TextDisabled("No samples yet...");
        ImGui::End();
        return;
    }

    const SignalHistory& H = itH->second;

    float newest_t = H.t.back();
    float oldest_allowed = newest_t - W.view_seconds;

    int start = 0;
    while (start < (int)H.t.size() && H.t[start] < oldest_allowed) start++;
    int count = (int)H.t.size() - start;

    if (count <= 1) { ImGui::TextDisabled("Not enough samples..."); ImGui::End(); return; }

    static thread_local std::vector<float> ttmp;
    static thread_local std::vector<float> ytmp;
    ttmp.clear(); ytmp.clear();
    ttmp.reserve((size_t)count);
    ytmp.reserve((size_t)count);
    for (int i = start; i < (int)H.y.size(); ++i) {
        ttmp.push_back(H.t[i]);
        ytmp.push_back(H.y[i]);
    }

    float y_min = 0.0f, y_max = 0.0f;
    if (W.autoscale) compute_minmax(ytmp.data(), (int)ytmp.size(), y_min, y_max);
    else {
        y_min = W.manual_min;
        y_max = W.manual_max;
        if (y_min == y_max) y_max = y_min + 1.0f;
    }

    float t_min = ttmp.front();
    float t_max = ttmp.back();
    if (t_min == t_max) t_max = t_min + 1.0f;

    ImGui::Checkbox("Auto scale", &W.autoscale);
    ImGui::SameLine();
    ImGui::SliderFloat("View (sec)", &W.view_seconds, 0.5f, 60.0f, "%.1f");
    ImGui::SameLine();
    ImGui::InputText("Y label", W.y_label, sizeof(W.y_label));

    if (!W.autoscale) {
        ImGui::DragFloat("Y min", &W.manual_min, 0.01f);
        ImGui::DragFloat("Y max", &W.manual_max, 0.01f);
    } else {
        ImGui::Text("Y range: [% .6f, % .6f]", y_min, y_max);
    }

    ImGui::Separator();

    DrawLabeledTimePlot("##plot",
                        ttmp.data(), ytmp.data(), (int)ytmp.size(),
                        t_min, t_max,
                        y_min, y_max,
                        "Time (s)",
                        W.y_label,
                        ImVec2(-1, 320));

    ImGui::End();
}

// ------------------ Command-only console UI ------------------
static void draw_command_console(bool* p_open) {
    if (!ImGui::Begin("Console###CONSOLE", p_open)) { ImGui::End(); return; }

    static bool autoscroll = true;
    static char cmd_buf[256] = {0};
    static bool focus_cmd = false;

    ImGui::Checkbox("Auto-scroll", &autoscroll);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        std::lock_guard<std::mutex> lk(g_cmdlog_mtx);
        g_cmdlog.clear();
    }

    ImGui::Separator();

    ImGui::BeginChild("##cmdlog", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2), true);
    {
        std::lock_guard<std::mutex> lk(g_cmdlog_mtx);
        for (const auto& line : g_cmdlog) ImGui::TextUnformatted(line.c_str());
    }
    if (autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::TextUnformatted("Send command:");
    ImGui::SameLine();

    if (focus_cmd) {
        ImGui::SetKeyboardFocusHere();
        focus_cmd = false;
    }

    bool enter = ImGui::InputText("##cmd", cmd_buf, sizeof(cmd_buf), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    bool clicked = ImGui::Button("Send");

    if (enter || clicked) {
        std::string cmd(cmd_buf);
        if (!cmd.empty()) {
            bool ok = send_text_line(cmd);
            cmdlog_push(std::string("> ") + cmd);
            cmdlog_push(ok ? "  (sent)" : "  (FAILED to send)");
            cmd_buf[0] = 0;
        }
        focus_cmd = true; // keep focus after send
    }

    ImGui::End();
}

// ------------------ Main ------------------
int main(int argc, char** argv) {
    std::string port =
#ifdef _WIN32
        (argc >= 2) ? argv[1] : "COM5";
#else
        (argc >= 2) ? argv[1] : "/dev/ttyACM0";
#endif

    std::thread th(serial_thread_fn, port);

    if (!glfwInit()) return 1;
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(1200, 780, "RTE", nullptr, nullptr);
    if (!window) return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    char filter[128] = {0};
    bool console_open = true;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        TelemetryState snap;
        {
            std::lock_guard<std::mutex> lk(g_state_mtx);
            snap = g_state;
        }

        ImGui::Begin("RTE");

        ImGui::Text("Port: %s", port.c_str());
        ImGui::SameLine();
        ImGui::Text("RX: %.1f Hz", snap.rx_hz);
        ImGui::SameLine();
        ImGui::Text("Seq: %u", snap.last_seq);

        ImGui::Text("Good: %llu   Bad: %llu",
            (unsigned long long)snap.good_frames,
            (unsigned long long)snap.bad_frames);

        ImGui::Text("Reject: decode=%llu  crc=%llu  hdr=%llu  len=%llu",
            (unsigned long long)g_reject_decode.load(),
            (unsigned long long)g_reject_crc.load(),
            (unsigned long long)g_reject_hdr.load(),
            (unsigned long long)g_reject_len.load());

        ImGui::Separator();

        ImGui::TextUnformatted("History retention (global)");
        ImGui::SliderFloat("Retain (sec)", &g_retain_seconds, 2.0f, 120.0f, "%.1f");
        ImGui::SliderInt("Max samples cap", &g_max_samples_cap, 500, 50000);

        ImGui::Separator();
        ImGui::InputTextWithHint("Filter", "type to filter signals...", filter, sizeof(filter));
        ImGui::SameLine();
        if (ImGui::Button(console_open ? "Hide Console" : "Show Console")) console_open = !console_open;

        ImGui::Separator();

        std::vector<std::string> keys;
        keys.reserve(snap.latest.size());
        for (auto& kv : snap.latest) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());

        ImGui::BeginChild("##signal_list", ImVec2(0, 0), true);
        for (auto& k : keys) {
            if (filter[0] != 0 && k.find(filter) == std::string::npos) continue;

            float v = snap.latest[k];
            bool pinned = (find_pin_index(k) >= 0);

            ImGui::PushID(k.c_str());
            ImGui::TextUnformatted(k.c_str());
            ImGui::SameLine(260);
            ImGui::Text("% .6f", v);
            ImGui::SameLine(430);

            if (!pinned) {
                if (ImGui::SmallButton("Open Plot")) pin_signal(k);
            } else {
                if (ImGui::SmallButton("Close Plot")) {
                    int idx = find_pin_index(k);
                    if (idx >= 0) remove_pin_at(idx);
                }
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::End();

        if (console_open) draw_command_console(&console_open);

        for (auto& w : g_pins) draw_signal_window(w, snap);
        for (int i = (int)g_pins.size() - 1; i >= 0; --i)
            if (!g_pins[i].open) remove_pin_at(i);

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    g_run.store(false);
    if (th.joinable()) th.join();

    {
        std::lock_guard<std::mutex> lk(g_serial_mtx);
        if (g_serial != INVALID_SERIAL) {
            serial_close(g_serial);
            g_serial = INVALID_SERIAL;
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
