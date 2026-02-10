#include "telemetry_protocol.h"
#include <thread>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <algorithm>

#ifndef _WIN32
  #include <fcntl.h>
  #include <unistd.h>
  #include <termios.h>
  #include <sys/ioctl.h>
#endif

static constexpr uint32_t MAGIC = 0x544C4D31u; // "TLM1"
static constexpr uint8_t  VERSION = 1;
static constexpr uint8_t  MSG_TELEMETRY = 1;

static uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

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

// ---------------- SerialPort ----------------
SerialPort::~SerialPort() { close(); }

bool SerialPort::isOpen() const {
#ifdef _WIN32
    return h_ != INVALID_HANDLE_VALUE;
#else
    return h_ >= 0;
#endif
}

void SerialPort::close() {
#ifdef _WIN32
    if (h_ != INVALID_HANDLE_VALUE) {
        CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }
#else
    if (h_ >= 0) {
        ::close(h_);
        h_ = -1;
    }
#endif
}

bool SerialPort::open(const std::string& port, int baud) {
    (void)baud; // fixed at 115200 below
    close();

#ifdef _WIN32
    std::string full = "\\\\.\\" + port;
    HANDLE h = CreateFileA(full.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DCB dcb{}; dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) { CloseHandle(h); return false; }

    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(h, &dcb)) { CloseHandle(h); return false; }

    COMMTIMEOUTS t{};
    t.ReadIntervalTimeout = 1;
    t.ReadTotalTimeoutConstant = 1;
    SetCommTimeouts(h, &t);

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    h_ = h;
    return true;
#else
    int fd = ::open(port.c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) return false;

    termios tty{};
    if (tcgetattr(fd, &tty) != 0) { ::close(fd); return false; }

    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    cfmakeraw(&tty);

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cc[VMIN]  = 1;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) { ::close(fd); return false; }

    // assert DTR/RTS
    int flags = 0;
    if (ioctl(fd, TIOCMGET, &flags) != -1) {
        flags |= (TIOCM_DTR | TIOCM_RTS);
        ioctl(fd, TIOCMSET, &flags);
    }

    tcflush(fd, TCIFLUSH);
    h_ = fd;
    return true;
#endif
}

int SerialPort::read(uint8_t* buf, int cap) {
    if (!isOpen()) return 0;
#ifdef _WIN32
    DWORD got = 0;
    if (!ReadFile(h_, buf, (DWORD)cap, &got, nullptr)) return 0;
    return (int)got;
#else
    int n = (int)::read(h_, buf, (size_t)cap);
    return n > 0 ? n : 0;
#endif
}

bool SerialPort::write(const uint8_t* data, int n) {
    if (!isOpen()) return false;
#ifdef _WIN32
    DWORD wrote = 0;
    if (!WriteFile(h_, data, (DWORD)n, &wrote, nullptr)) return false;
    return (int)wrote == n;
#else
    int wrote = (int)::write(h_, data, (size_t)n);
    return wrote == n;
#endif
}

// ---------------- TelemetryClient ----------------
struct TelemetryClient::ThreadImpl {
    std::thread t;
};

static inline void set_latest(TelemetryState& S, const char* name, float v) {
    S.latest[name] = v;
}

bool TelemetryClient::start(const std::string& port) {
    stop();
    run_.store(true);

    thr_ = new ThreadImpl();
    thr_->t = std::thread(&TelemetryClient::threadMain, this, port);
    return true;
}

void TelemetryClient::stop() {
    run_.store(false);
    if (thr_) {
        if (thr_->t.joinable()) thr_->t.join();
        delete thr_;
        thr_ = nullptr;
    }
    std::lock_guard<std::mutex> lk(serial_mtx_);
    serial_.close();
}

TelemetryState TelemetryClient::snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return st_;
}

bool TelemetryClient::sendLine(const std::string& line) {
    std::string out = line;
    if (out.empty()) return false;
    if (out.back() != '\n' && out.back() != '\r') out.push_back('\n');

    std::lock_guard<std::mutex> lk(serial_mtx_);
    return serial_.write((const uint8_t*)out.data(), (int)out.size());
}

void TelemetryClient::trimHistoryLocked(SignalHistory& H) {
    const float keep = retain_seconds_.load();
    const int cap = max_samples_.load();

    if (!H.t.empty()) {
        float newest = H.t.back();
        float oldest_allowed = newest - keep;
        while (!H.t.empty() && H.t.front() < oldest_allowed) {
            H.t.pop_front();
            H.y.pop_front();
        }
    }
    while ((int)H.t.size() > cap) {
        H.t.pop_front();
        H.y.pop_front();
    }
}

void TelemetryClient::ingestLocked(const TelemetryHeader& h, const TelemetryPayloadV1& p, float tsec) {
    auto& S = st_;
    auto push = [&](const char* name, float v) {
        S.latest[name] = v;
        auto& H = S.hist[name];
        H.t.push_back(tsec);
        H.y.push_back(v);
        trimHistoryLocked(H);
    };

    push("V_DC_BUS", p.v_dc);
    push("V_PH_U",   p.v_u);
    push("V_PH_V",   p.v_v);
    push("V_PH_W",   p.v_w);
    push("I_DC_MAIN", p.i_dc_main);
    push("I_PH_U",    p.i_u);
    push("I_PH_W",    p.i_w);
    push("ENCODER_SIN", p.enc_sin);
    push("ENCODER_COS", p.enc_cos);
    push("ROTOR_DEG",   p.rotor_deg);
    push("SENSOR_RATE_KHZ", p.sensor_rate_khz);

    S.last_seq = h.seq;
    S.good_frames++;
}
void TelemetryClient::threadMain(const std::string& port) {
    auto reopen_and_settle = [&](bool first_time) {
        {
            std::lock_guard<std::mutex> lk(serial_mtx_);
            serial_.close();
        }

        // On some platforms/USB CDC, a short delay helps avoid "open but dead" states
        std::this_thread::sleep_for(std::chrono::milliseconds(first_time ? 200 : 150));

        while (run_.load()) {
            bool ok = false;
            {
                std::lock_guard<std::mutex> lk(serial_mtx_);
                ok = serial_.open(port, 115200);
            }
            if (ok) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!run_.load()) return false;

        // Settle time after open (CDC ACM often resets MCU)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Flush any junk/partial bytes
        {
            std::lock_guard<std::mutex> lk(serial_mtx_);
            uint8_t junk[256];
            for (int i = 0; i < 10; ++i) (void)serial_.read(junk, (int)sizeof(junk));
        }
        return true;
    };

    if (!reopen_and_settle(true)) return;

    std::vector<uint8_t> frame;
    frame.reserve(256);

    uint8_t decoded[256];
    uint8_t buf[512];

    auto t0 = std::chrono::steady_clock::now();
    uint64_t frames_in_window = 0;
    auto hz_window_start = t0;

    // NEW: last time we successfully decoded a valid telemetry frame
    auto last_good_frame = std::chrono::steady_clock::now();

    // NEW: parser reset helper (important after reopen)
    auto reset_parser = [&]() {
        frame.clear();
        frames_in_window = 0;
        hz_window_start = std::chrono::steady_clock::now();
        last_good_frame = std::chrono::steady_clock::now();
    };

    while (run_.load()) {
        // NEW: timeout-based retry if no good frames for 0.5s
        {
            auto now = std::chrono::steady_clock::now();
            if (now - last_good_frame > std::chrono::milliseconds(500)) {
                // attempt reopen
                if (!reopen_and_settle(false)) break;
                reset_parser();
            }
        }

        int n = 0;
        {
            std::lock_guard<std::mutex> lk(serial_mtx_);
            n = serial_.read(buf, (int)sizeof(buf));
        }
        if (n == 0) {
            // A small sleep prevents a hot spin if read returns 0 for any reason
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        for (int i = 0; i < n; ++i) {
            uint8_t b = buf[i];

            if (b == 0x00) {
                if (!frame.empty()) {
                    bool good = false;
                    try {
                        size_t dec_len = cobs_decode(frame.data(), frame.size(), decoded, sizeof(decoded));

                        if (dec_len < sizeof(TelemetryHeader) + 2) {
                            std::lock_guard<std::mutex> lk(mtx_);
                            st_.reject_len++; st_.bad_frames++;
                            throw std::runtime_error("short");
                        }

                        uint16_t rx_crc = (uint16_t)decoded[dec_len - 2] | ((uint16_t)decoded[dec_len - 1] << 8);
                        uint16_t calc   = crc16_ccitt(decoded, dec_len - 2);
                        if (rx_crc != calc) {
                            std::lock_guard<std::mutex> lk(mtx_);
                            st_.reject_crc++; st_.bad_frames++;
                            throw std::runtime_error("crc");
                        }

                        TelemetryHeader h{};
                        std::memcpy(&h, decoded, sizeof(h));
                        if (h.magic != MAGIC || h.version != VERSION || h.msg_type != MSG_TELEMETRY) {
                            std::lock_guard<std::mutex> lk(mtx_);
                            st_.reject_hdr++; st_.bad_frames++;
                            throw std::runtime_error("hdr");
                        }

                        if (h.payload_len != sizeof(TelemetryPayloadV1) ||
                            sizeof(TelemetryHeader) + h.payload_len + 2 != dec_len) {
                            std::lock_guard<std::mutex> lk(mtx_);
                            st_.reject_len++; st_.bad_frames++;
                            throw std::runtime_error("len");
                        }

                        TelemetryPayloadV1 p{};
                        std::memcpy(&p, decoded + sizeof(TelemetryHeader), sizeof(p));

                        auto now = std::chrono::steady_clock::now();
                        float tsec = std::chrono::duration<float>(now - t0).count();

                        {
                            std::lock_guard<std::mutex> lk(mtx_);
                            ingestLocked(h, p, tsec);
                        }

                        // NEW: mark as good frame received
                        last_good_frame = std::chrono::steady_clock::now();
                        good = true;

                        frames_in_window++;
                        float dt = std::chrono::duration<float>(now - hz_window_start).count();
                        if (dt >= 1.0f) {
                            float hz = frames_in_window / dt;
                            std::lock_guard<std::mutex> lk(mtx_);
                            st_.rx_hz = hz;
                            frames_in_window = 0;
                            hz_window_start = now;
                        }
                    } catch (...) {
                        // keep going; bad frames happen during reconnect/startup
                    }

                    (void)good;
                    frame.clear();
                }
            } else {
                if (frame.size() < 255) frame.push_back(b);
                else frame.clear(); // oversize -> drop
            }
        }
    }
}
