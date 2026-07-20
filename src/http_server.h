#pragma once
#include "firmware_updater.h"
#include "telemetry_protocol.h"
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

class HttpFlashServer {
public:
    HttpFlashServer(FirmwareUpdater& updater, TelemetryClient& telemetry,
                    std::string port = "8080");
    ~HttpFlashServer();

    bool start();
    bool start(const std::string& port);
    bool restart(const std::string& port);
    void stop();
    bool isRunning() const { return running_.load(); }
    int actualPort() const { return actual_port_.load(); }

    // Reported by GET /api/info so clients can find the on-disk logs.
    void setLogDir(const std::string& dir) { log_dir_ = dir; }

private:
    void threadMain();

    FirmwareUpdater& updater_;
    TelemetryClient& telemetry_;
    std::string log_dir_;
    std::chrono::steady_clock::time_point start_time_{};
    std::string port_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<int> actual_port_{0};

#ifdef _WIN32
    uintptr_t listen_fd_ = (uintptr_t)~0;
#else
    int listen_fd_ = -1;
#endif

    std::thread thread_;
};
