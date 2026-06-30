#pragma once
#include "firmware_updater.h"
#include <atomic>
#include <string>
#include <thread>

class HttpFlashServer {
public:
    HttpFlashServer(FirmwareUpdater& updater, std::string port = "8080");
    ~HttpFlashServer();

    bool start();
    bool start(const std::string& port);
    bool restart(const std::string& port);
    void stop();
    bool isRunning() const { return running_.load(); }
    int actualPort() const { return actual_port_.load(); }

private:
    void threadMain();

    FirmwareUpdater& updater_;
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
