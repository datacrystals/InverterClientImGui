#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <atomic>

#ifdef _WIN32
  #include <windows.h>
  using SerialHandle = HANDLE;
  static constexpr SerialHandle INVALID_SERIAL = INVALID_HANDLE_VALUE;
#else
  using SerialHandle = int;
  static constexpr SerialHandle INVALID_SERIAL = -1;
#endif

#pragma pack(push, 1)
struct TelemetryHeader {
    uint32_t magic;       // "TLM1" = 0x544C4D31
    uint8_t  version;     // 1
    uint8_t  msg_type;    // 1 = telemetry
    uint16_t payload_len; // bytes
    uint32_t seq;
    uint32_t time_us;     // pico time_us_32()
};

struct TelemetryPayloadV1 {
    float v_dc, v_u, v_v, v_w;
    float i_dc_main, i_u, i_w;
    float enc_sin, enc_cos, rotor_deg;
    float sensor_rate_khz;
};
#pragma pack(pop)

struct SignalHistory {
    std::deque<float> t;
    std::deque<float> y;
};

struct TelemetryState {
    std::unordered_map<std::string, float> latest;
    std::unordered_map<std::string, SignalHistory> hist;

    uint32_t last_seq = 0;
    float rx_hz = 0.0f;
    uint64_t good_frames = 0;
    uint64_t bad_frames  = 0;

    uint64_t reject_decode = 0;
    uint64_t reject_crc    = 0;
    uint64_t reject_hdr    = 0;
    uint64_t reject_len    = 0;
};

class SerialPort {
public:
    SerialPort() = default;
    ~SerialPort();

    bool open(const std::string& port, int baud = 115200);
    void close();
    bool isOpen() const;

    int  read(uint8_t* buf, int cap);
    bool write(const uint8_t* data, int n);

private:
    SerialHandle h_ = INVALID_SERIAL;
};

class TelemetryClient {
public:
    bool start(const std::string& port);
    void stop();

    // Thread-safe snapshot copy
    TelemetryState snapshot() const;

    // Send ASCII command (adds '\n' if needed)
    bool sendLine(const std::string& line);

    // Settings (thread-safe via atomics)
    void setRetainSeconds(float s) { retain_seconds_.store(s); }
    void setMaxSamples(int n)      { max_samples_.store(n); }

private:
    void threadMain(const std::string& port);
    void ingestLocked(const TelemetryHeader& h, const TelemetryPayloadV1& p, float tsec);
    void trimHistoryLocked(SignalHistory& H);

private:
    mutable std::mutex mtx_;
    TelemetryState st_;

    std::atomic<bool> run_{false};
    std::atomic<float> retain_seconds_{30.0f};
    std::atomic<int>   max_samples_{12000};

    // reader thread
    struct ThreadImpl;
    ThreadImpl* thr_ = nullptr;

    // serial
    mutable std::mutex serial_mtx_;
    SerialPort serial_;
};
