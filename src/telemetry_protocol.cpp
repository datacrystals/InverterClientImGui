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

// ---------------- CRC16-CCITT (0x1021, init 0xFFFF) ----------------
static uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// ---------------- COBS decode ----------------
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

// ---------------- Payload decoding helpers ----------------
static inline uint16_t rd_u16(const uint8_t*& p, const uint8_t* end) {
    if (end - p < 2) throw std::runtime_error("u16");
    uint16_t v = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    p += 2;
    return v;
}
static inline uint8_t rd_u8(const uint8_t*& p, const uint8_t* end) {
    if (end - p < 1) throw std::runtime_error("u8");
    return *p++;
}
static inline float rd_f32(const uint8_t*& p, const uint8_t* end) {
    if (end - p < 4) throw std::runtime_error("f32");
    float f;
    std::memcpy(&f, p, 4);
    p += 4;
    return f;
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
    (void)baud; // fixed at 460800 below
    close();

#ifdef _WIN32
    std::string full = "\\\\.\\" + port;
    HANDLE h = CreateFileA(full.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DCB dcb{}; dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) { CloseHandle(h); return false; }

    dcb.BaudRate = 460800;
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

cfsetispeed(&tty, B460800);
cfsetospeed(&tty, B460800);
cfmakeraw(&tty);

tty.c_cflag |= (CLOCAL | CREAD);
tty.c_cflag &= ~CRTSCTS;
tty.c_cc[VMIN]  = 0;
tty.c_cc[VTIME] = 1;

if (tcsetattr(fd, TCSANOW, &tty) != 0) { ::close(fd); return false; }

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
    if (!isOpen() || !data || n <= 0) return false;
#ifdef _WIN32
    int total = 0;
    while (total < n) {
        DWORD wrote = 0;
        if (!WriteFile(h_, data + total, (DWORD)(n - total), &wrote, nullptr)) return false;
        if (wrote == 0) return false;
        total += (int)wrote;
    }
    return true;
#else
    int total = 0;
    while (total < n) {
        int wrote = (int)::write(h_, data + total, (size_t)(n - total));
        if (wrote < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (wrote == 0) return false;
        total += wrote;
    }
    return true;
#endif
}

bool SerialPort::drain() {
    if (!isOpen()) return false;
#ifdef _WIN32
    return FlushFileBuffers(h_) != 0;
#else
    return ::tcdrain(h_) == 0;
#endif
}

// ---------------- TelemetryClient ----------------
static constexpr size_t   CONSOLE_CAP_LINES = 6000;
static uint64_t           g_console_seq = 0;
struct TelemetryClient::ThreadImpl {
    std::thread t;
};

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
    if (!serial_.write((const uint8_t*)out.data(), (int)out.size())) return false;
    return serial_.drain();
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

void TelemetryClient::ingestF32Locked(const std::string& key, float v, float tsec) {
    auto& S = st_;
    S.latest[key] = v;
    auto& H = S.hist[key];
    H.t.push_back(tsec);
    H.y.push_back(v);
    trimHistoryLocked(H);
}

void TelemetryClient::onDefineLocked(uint16_t id, uint8_t type, const char* key, uint8_t key_len) {
    if (!key || key_len == 0) return;
    KeyDef def;
    def.type = type;
    def.key.assign(key, key + key_len);
    id_to_key_[id] = std::move(def);
}

bool TelemetryClient::lookupKeyLocked(uint16_t id, KeyDef& out) const {
    auto it = id_to_key_.find(id);
    if (it == id_to_key_.end()) return false;
    out = it->second;
    return true;
}

// --------- NEW: member payload parsers (can access private members) ----------
void TelemetryClient::parseDefinePayloadLocked_(const uint8_t* payload, size_t len) {
    const uint8_t* p = payload;
    const uint8_t* end = payload + len;

    uint8_t n = rd_u8(p, end);

    for (uint8_t i = 0; i < n; ++i) {
        uint16_t id = rd_u16(p, end);
        uint8_t type = rd_u8(p, end);
        uint8_t klen = rd_u8(p, end);
        if ((size_t)(end - p) < klen) throw std::runtime_error("klen");
        onDefineLocked(id, type, (const char*)p, klen);
        p += klen;
    }
}

void TelemetryClient::parseDataPayloadLocked_(const uint8_t* payload, size_t len, float tsec) {
    const uint8_t* p = payload;
    const uint8_t* end = payload + len;

    uint8_t n = rd_u8(p, end);

    for (uint8_t i = 0; i < n; ++i) {
        uint16_t id = rd_u16(p, end);
        uint8_t wire_type = rd_u8(p, end);

        KeyDef def;
        if (!lookupKeyLocked(id, def)) {
            st_.reject_unknown_id++;
            // Skip value based on wire_type (so we stay in sync)
            if (wire_type == VT_F32) {
                (void)rd_f32(p, end);
            } else if (wire_type == VT_STR) {
                uint8_t sl = rd_u8(p, end);
                if ((size_t)(end - p) < sl) throw std::runtime_error("str");
                p += sl;
            } else if (wire_type == VT_STR_FRAG) {
                (void)rd_u8(p, end); // frag flags
                uint8_t sl = rd_u8(p, end);
                if ((size_t)(end - p) < sl) throw std::runtime_error("str frag");
                p += sl;
            } else {
                throw std::runtime_error("bad type");
            }
            continue;
        }

        // Trust DEFINE type, but still advance using the on-wire encoding.
        if (def.type == VT_F32) {
            // If wire_type isn't VT_F32, try to skip safely and count it.
            if (wire_type != VT_F32) {
                st_.reject_payload_parse++;
                if (wire_type == VT_STR) {
                    uint8_t sl = rd_u8(p, end);
                    if ((size_t)(end - p) < sl) throw std::runtime_error("str");
                    p += sl;
                } else if (wire_type == VT_STR_FRAG) {
                    (void)rd_u8(p, end); // frag flags
                    uint8_t sl = rd_u8(p, end);
                    if ((size_t)(end - p) < sl) throw std::runtime_error("str frag");
                    p += sl;
                } else {
                    throw std::runtime_error("wire type");
                }
                continue;
            }
            float v = rd_f32(p, end);
            ingestF32Locked(def.key, v, tsec);
        } else if (def.type == VT_STR) {
            if (wire_type == VT_F32) {
                st_.reject_payload_parse++;
                (void)rd_f32(p, end);
                continue;
            }
            if (wire_type != VT_STR && wire_type != VT_STR_FRAG) {
                st_.reject_payload_parse++;
                throw std::runtime_error("wire type");
            }

            if (wire_type == VT_STR) {
                uint8_t sl = rd_u8(p, end);
                if ((size_t)(end - p) < sl) throw std::runtime_error("str");
                std::string s((const char*)p, (const char*)p + sl);
                p += sl;
                partial_str_.erase(def.key);
                ingestStrLocked(def.key, s);
            } else { // VT_STR_FRAG
                uint8_t frag = rd_u8(p, end);
                uint8_t sl = rd_u8(p, end);
                if ((size_t)(end - p) < sl) throw std::runtime_error("str frag");
                std::string chunk((const char*)p, (const char*)p + sl);
                p += sl;

                auto& part = partial_str_[def.key];
                if (frag & SF_START) {
                    part.buf.clear();
                }
                part.buf += chunk;
                part.last_tsec = tsec;

                if (frag & SF_END) {
                    ingestStrLocked(def.key, part.buf);
                    partial_str_.erase(def.key);
                }
            }
        } else {
            st_.reject_payload_parse++;
            throw std::runtime_error("unknown def type");
        }
    }
}

// NOTE: these are private helpers, but we didn't declare them in the header yet.
// Add these two private declarations to TelemetryClient in telemetry_protocol.h:
//
//   void parseDefinePayloadLocked_(const uint8_t* payload, size_t len);
//   void parseDataPayloadLocked_(const uint8_t* payload, size_t len, float tsec);
//
// (Or, if you prefer, inline the parsing directly into threadMain under the lock.)
void TelemetryClient::ingestStrLocked(const std::string& key, const std::string& v) {
    st_.latest_str[key] = v;

    if (key == "print") {
        st_.console.push_back(ConsoleLine{++g_console_seq, v});
        while (st_.console.size() > CONSOLE_CAP_LINES) st_.console.pop_front();
    }
}

void TelemetryClient::threadMain(const std::string& port) {
    auto reopen_and_settle = [&](bool first_time) {
        {
            std::lock_guard<std::mutex> lk(serial_mtx_);
            serial_.close();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(first_time ? 200 : 150));

        while (run_.load()) {
            bool ok = false;
            {
                std::lock_guard<std::mutex> lk(serial_mtx_);
                ok = serial_.open(port, 460800);
            }
            if (ok) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!run_.load()) return false;

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // {
        //     std::lock_guard<std::mutex> lk(serial_mtx_);
        //     uint8_t junk[256];
        //     for (int i = 0; i < 10; ++i) (void)serial_.read(junk, (int)sizeof(junk));
        // }
        return true;
    };

    if (!reopen_and_settle(true)) return;

    std::vector<uint8_t> frame;
    frame.reserve(1024);

    uint8_t decoded[4096];
    uint8_t buf[512];

    auto t0 = std::chrono::steady_clock::now();
    uint64_t frames_in_window = 0;
    auto hz_window_start = t0;

    uint64_t bytes_in_window = 0;
    auto bytes_window_start = t0;

    auto last_good_frame = std::chrono::steady_clock::now();

    auto reset_parser = [&]() {
        frame.clear();
        frames_in_window = 0;
        hz_window_start = std::chrono::steady_clock::now();
        bytes_in_window = 0;
        bytes_window_start = std::chrono::steady_clock::now();
        last_good_frame = std::chrono::steady_clock::now();
    };

    while (run_.load()) {
        {
            auto now = std::chrono::steady_clock::now();
            if (now - last_good_frame > std::chrono::seconds(2)) {
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
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        bytes_in_window += (uint64_t)n;

        for (int i = 0; i < n; ++i) {
            uint8_t b = buf[i];

            if (b == 0x00) {
                if (!frame.empty()) {
                    bool parsed_ok = false;
                    try {
                        size_t dec_len = cobs_decode(frame.data(), frame.size(), decoded, sizeof(decoded));

                        if (dec_len < sizeof(TelemetryHeader) + 2) {
                            std::lock_guard<std::mutex> lk(mtx_);
                            st_.reject_len++; st_.bad_frames++;
                            throw std::runtime_error("short");
                        }

                        uint16_t rx_crc = (uint16_t)decoded[dec_len - 2] |
                                          ((uint16_t)decoded[dec_len - 1] << 8);
                        uint16_t calc = crc16_ccitt(decoded, dec_len - 2);
                        if (rx_crc != calc) {
                            std::lock_guard<std::mutex> lk(mtx_);
                            st_.reject_crc++; st_.bad_frames++;
                            throw std::runtime_error("crc");
                        }

                        TelemetryHeader h{};
                        std::memcpy(&h, decoded, sizeof(h));
                        if (h.magic != MAGIC || h.version != VERSION ||
                            (h.msg_type != MSG_DATA && h.msg_type != MSG_DEFINE)) {
                            std::lock_guard<std::mutex> lk(mtx_);
                            st_.reject_hdr++; st_.bad_frames++;
                            throw std::runtime_error("hdr");
                        }

                        if (sizeof(TelemetryHeader) + h.payload_len + 2 != dec_len) {
                            std::lock_guard<std::mutex> lk(mtx_);
                            st_.reject_len++; st_.bad_frames++;
                            throw std::runtime_error("len");
                        }

                        auto now = std::chrono::steady_clock::now();
                        float tsec = std::chrono::duration<float>(now - t0).count();

                        const uint8_t* payload = decoded + sizeof(TelemetryHeader);
                        const size_t plen = h.payload_len;

                        {
                            std::lock_guard<std::mutex> lk(mtx_);
                            if (h.msg_type == MSG_DEFINE) {
                                parseDefinePayloadLocked_(payload, plen);
                            } else {
                                parseDataPayloadLocked_(payload, plen, tsec);
                            }

                            // Drop partial strings that haven't seen a fragment in 2s
                            for (auto it = partial_str_.begin(); it != partial_str_.end(); ) {
                                if (tsec - it->second.last_tsec > 2.0f) {
                                    it = partial_str_.erase(it);
                                } else {
                                    ++it;
                                }
                            }

                            st_.last_seq = h.seq;
                            st_.good_frames++;
                        }

                        parsed_ok = true;
                        last_good_frame = std::chrono::steady_clock::now();

                        frames_in_window++;
                        float dt = std::chrono::duration<float>(now - hz_window_start).count();
                        if (dt >= 1.0f) {
                            float hz = frames_in_window / dt;
                            float bytes_per_sec = bytes_in_window / dt;
                            std::lock_guard<std::mutex> lk(mtx_);
                            st_.rx_hz = hz;
                            st_.rx_bytes_per_sec = bytes_per_sec;
                            frames_in_window = 0;
                            hz_window_start = now;
                            bytes_in_window = 0;
                            bytes_window_start = now;
                        }
                    } catch (...) {
                        // keep going; bad frames happen during reconnect/startup
                        std::lock_guard<std::mutex> lk(mtx_);
                        // st_.bad_frames++;
                        // reject counters already bumped for crc/hdr/len. If it was payload parse, mark it:
                        if (!parsed_ok) st_.reject_payload_parse++;
                    }

                    frame.clear();
                }
            } else {
                if (frame.size() < 4096) frame.push_back(b);
                else frame.clear(); // oversize -> drop
            }
        }
    }
}
