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

static constexpr uint32_t MAGIC   = 0x544C4D31u; // "TLM1"
static constexpr uint8_t  VERSION = 1;

enum MsgType : uint8_t {
    MSG_DATA   = 1,
    MSG_DEFINE = 2,
};

enum ValueType : uint8_t {
    VT_F32      = 1,
    VT_STR      = 2,   // complete short string
    VT_STR_FRAG = 3,   // fragment of a longer string
};

// Fragment flags for VT_STR_FRAG payloads
enum StrFrag : uint8_t {
    SF_START    = 0x01,
    SF_END      = 0x02,
    SF_COMPLETE = 0x03, // START | END
};

#pragma pack(push, 1)
struct TelemetryHeader {
    uint32_t magic;       // MAGIC
    uint8_t  version;     // VERSION
    uint8_t  msg_type;    // MsgType
    uint16_t payload_len; // bytes
    uint32_t seq;
    uint32_t time_us;     // pico time_us_32()
};
#pragma pack(pop)

struct SignalHistory {
    std::deque<float> t;
    std::deque<float> y;
};
struct ConsoleLine {
    uint64_t seq = 0;
    std::string text;
};
struct TelemetryState {
    std::deque<ConsoleLine> console;
    // Float signals only (for plotting)
    std::unordered_map<std::string, float> latest;
    std::unordered_map<std::string, SignalHistory> hist;

    // String signals (latest only)
    std::unordered_map<std::string, std::string> latest_str;

    uint32_t last_seq = 0;
    float rx_hz = 0.0f;
    uint64_t good_frames = 0;
    uint64_t bad_frames  = 0;

    uint64_t reject_decode = 0;
    uint64_t reject_crc    = 0;
    uint64_t reject_hdr    = 0;
    uint64_t reject_len    = 0;

    // New: protocol-level rejects
    uint64_t reject_unknown_id = 0;
    uint64_t reject_payload_parse = 0;
};

class SerialPort {
public:
    SerialPort() = default;
    ~SerialPort();

    bool open(const std::string& port, int baud = 460800);
    void close();
    bool isOpen() const;

    int  read(uint8_t* buf, int cap);
    bool write(const uint8_t* data, int n);
    bool drain();

private:
    SerialHandle h_ = INVALID_SERIAL;
};

class TelemetryClient {
public:
    bool start(const std::string& port);
    void stop();

    TelemetryState snapshot() const;
    void ingestStrLocked(const std::string& key, const std::string& v);
    bool sendLine(const std::string& line);

    void setRetainSeconds(float s) { retain_seconds_.store(s); }
    void setMaxSamples(int n)      { max_samples_.store(n); }

private:
    void threadMain(const std::string& port);

    // ingest helpers (caller holds mtx_)
    void ingestF32Locked(const std::string& key, float v, float tsec);

    void trimHistoryLocked(SignalHistory& H);

    // NEW: dynamic key registry (id -> (type,key))
    struct KeyDef {
        uint8_t type = 0;
        std::string key;
    };
    void onDefineLocked(uint16_t id, uint8_t type, const char* key, uint8_t key_len);
    bool lookupKeyLocked(uint16_t id, KeyDef& out) const;

private:
    mutable std::mutex mtx_;
    void parseDefinePayloadLocked_(const uint8_t* payload, size_t len);
void parseDataPayloadLocked_(const uint8_t* payload, size_t len, float tsec);

    TelemetryState st_;
    std::unordered_map<uint16_t, KeyDef> id_to_key_; // guarded by mtx_

    // Long-string reassembly (guarded by mtx_)
    struct PartialString {
        std::string buf;
        float last_tsec = 0.0f;
    };
    std::unordered_map<std::string, PartialString> partial_str_;

    std::atomic<bool> run_{false};
    std::atomic<float> retain_seconds_{30.0f};
    std::atomic<int>   max_samples_{12000};

    struct ThreadImpl;
    ThreadImpl* thr_ = nullptr;

    mutable std::mutex serial_mtx_;
    SerialPort serial_;
};
