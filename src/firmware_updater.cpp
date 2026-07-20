#include "firmware_updater.h"
#include "telemetry_protocol.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#define POPEN  _popen
#define PCLOSE _pclose
#define PATH_SEP ';'
#else
#include <fcntl.h>
#include <unistd.h>
#define POPEN  popen
#define PCLOSE pclose
#define PATH_SEP ':'
#endif

namespace fs = std::filesystem;

static constexpr size_t LOG_CAP = 200;
static constexpr uint32_t FLASH_BAUDRATE = 230400;
static constexpr const char* FLASH_ADDR = "0x08000000";

FirmwareUpdater::FirmwareUpdater() {
    thread_ = std::thread(&FirmwareUpdater::threadMain, this);
}

FirmwareUpdater::~FirmwareUpdater() {
    run_.store(false);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool FirmwareUpdater::queueFlash(const FlashJob& job, bool allow_queue) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (busy_) {
            if (!allow_queue) return false;
            // queue up to one pending job
            if (!queue_.empty()) return false;
            queue_.push_back(job);
            cv_.notify_all();
            return true;
        }
        queue_.push_back(job);
        cv_.notify_all();
    }
    return true;
}

FlashStatus FirmwareUpdater::status() const {
    std::lock_guard<std::mutex> lk(mtx_);
    FlashStatus s;
    s.state = state_;
    s.busy = busy_;
    s.last_error = last_error_;
    s.log = log_;
    return s;
}

const char* FirmwareUpdater::stateString(FlashState s) {
    switch (s) {
        case FlashState::Idle:          return "Idle";
        case FlashState::Draining:      return "Draining serial";
        case FlashState::EnteringBoot:  return "Entering bootloader";
        case FlashState::Flashing:      return "Flashing";
        case FlashState::Verifying:     return "Verifying";
        case FlashState::ExitingBoot:   return "Exiting bootloader";
        case FlashState::Done:          return "Done";
        case FlashState::Failed:        return "Failed";
    }
    return "Unknown";
}

void FirmwareUpdater::setCurrentPort(const std::string& port) {
    std::lock_guard<std::mutex> lk(mtx_);
    current_port_ = port;
}

std::string FirmwareUpdater::currentPort() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return current_port_;
}

void FirmwareUpdater::setSuspendCallback(std::function<void(bool)> cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    suspend_cb_ = std::move(cb);
}

void FirmwareUpdater::setState(FlashState s) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ = s;
    }
    logLine(std::string("[state] ") + stateString(s));
}

void FirmwareUpdater::logLine(const std::string& line) {
    std::lock_guard<std::mutex> lk(mtx_);
    log_.push_back(line);
    while (log_.size() > LOG_CAP) log_.erase(log_.begin());
}

void FirmwareUpdater::logStderr(const char* buf) {
    // split on newlines
    std::string s(buf ? buf : "");
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find('\n', start);
        if (end == std::string::npos) end = s.size();
        std::string line = s.substr(start, end - start);
        // strip trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) logLine(line);
        start = end + 1;
    }
}

bool FirmwareUpdater::findCli(std::string& out_cli) const {
    const char* candidates[] = {
        "/opt/st/stm32cubeclt_1.21.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI",
        "/opt/st/stm32cubeclt/STM32CubeProgrammer/bin/STM32_Programmer_CLI",
        "STM32_Programmer_CLI",
#ifdef _WIN32
        "C:\\Program Files\\STMicroelectronics\\STM32Cube\\STM32CubeProgrammer\\bin\\STM32_Programmer_CLI.exe",
#endif
        nullptr
    };

    for (const char** p = candidates; *p; ++p) {
        std::error_code ec;
        fs::path path(*p);
        // If it has a directory component, check existence directly.
        if (path.has_parent_path()) {
            if (fs::exists(path, ec) && !fs::is_directory(path, ec)) {
                out_cli = fs::canonical(path, ec).string();
                if (out_cli.empty()) out_cli = path.string();
                return true;
            }
        } else {
            // Search PATH
            const char* path_env = std::getenv("PATH");
            if (path_env) {
                std::stringstream ss(path_env);
                std::string dir;
                while (std::getline(ss, dir, PATH_SEP)) {
                    fs::path full = fs::path(dir) / path.filename();
                    if (fs::is_regular_file(full, ec)) {
                        out_cli = full.string();
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static fs::path exeDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n && n < MAX_PATH) {
        return fs::path(buf).parent_path();
    }
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return fs::path(buf).parent_path();
    }
#endif
    return {};
}

// Find a python interpreter that has EasyMCP2221 installed. Prefers the
// project .venv (created with: python3 -m venv .venv && .venv/bin/pip
// install EasyMCP2221); falls back to plain python3.
static std::string findPython() {
    std::error_code ec;

    if (const char* env = std::getenv("RTE_PYTHON")) {
        if (fs::is_regular_file(env, ec)) return env;
    }

    fs::path exe_dir = exeDir();
    std::vector<fs::path> candidates;
#ifdef _WIN32
    const char* venv_py[] = {".venv/Scripts/python.exe"};
#else
    const char* venv_py[] = {".venv/bin/python"};
#endif
    for (const char* rel : venv_py) {
        if (!exe_dir.empty()) {
            candidates.push_back(exe_dir / rel);
            candidates.push_back(exe_dir.parent_path() / rel);
        }
        candidates.push_back(fs::current_path(ec) / rel);
        candidates.push_back(rel);
    }

    for (const auto& p : candidates) {
        if (fs::is_regular_file(p, ec)) {
            // Do NOT canonicalize: a venv python is a symlink to the system
            // interpreter, and resolving it would drop the venv site-packages.
            std::string c = fs::absolute(p, ec).string();
            return c.empty() ? p.string() : c;
        }
    }
    return "python3";
}

bool FirmwareUpdater::findGpioHelper(std::string& out_helper) const {
    std::error_code ec;
    fs::path exe_dir = exeDir();

    std::vector<fs::path> candidates;
    if (!exe_dir.empty()) {
        candidates.push_back(exe_dir / "tools" / "mcp2221a_gpio.py");
        candidates.push_back(exe_dir.parent_path() / "tools" / "mcp2221a_gpio.py");
    }
    candidates.push_back(fs::current_path(ec) / "tools" / "mcp2221a_gpio.py");
    candidates.push_back("tools/mcp2221a_gpio.py");

    for (const auto& p : candidates) {
        if (fs::is_regular_file(p, ec)) {
            out_helper = fs::canonical(p, ec).string();
            if (out_helper.empty()) out_helper = p.string();
            return true;
        }
    }
    return false;
}

bool FirmwareUpdater::drainSerial(const std::string& port) {
    setState(FlashState::Draining);
    SerialPort sp;
    if (!sp.open(port, FLASH_BAUDRATE)) {
        logLine("[drain] Failed to open port " + port);
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    uint8_t junk[4096];
    int total = 0;
    for (int i = 0; i < 20 && run_.load(); ++i) {
        int n = sp.read(junk, (int)sizeof(junk));
        if (n > 0) total += n;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    sp.close();
    logLine(std::string("[drain] Drained ") + std::to_string(total) + " stale byte(s)");
    return true;
}

bool FirmwareUpdater::runCommand(const std::string& cmd, const std::string& desc) {
    logLine(std::string("$ ") + cmd);
    FILE* f = POPEN(cmd.c_str(), "r");
    if (!f) {
        last_error_ = desc + ": failed to run command";
        logLine(last_error_);
        return false;
    }
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) {
        logStderr(buf);
    }
    int rc = PCLOSE(f);
    if (rc != 0) {
        last_error_ = desc + ": command exited with code " + std::to_string(rc);
        logLine(last_error_);
        return false;
    }
    return true;
}

bool FirmwareUpdater::gpioCommand(const std::string& helper, const std::string& arg) {
    static const std::string python = findPython();
    std::string cmd = "\"" + python + "\" \"" + helper + "\" " + arg;
    return runCommand(cmd, std::string("GPIO ") + arg);
}

static void call_suspend_cb(const std::function<void(bool)>& cb, bool suspend) {
    if (cb) {
        try {
            cb(suspend);
        } catch (...) {
            // ignore callback errors; flash can still proceed
        }
    }
}

struct SuspendGuard {
    std::function<void(bool)> cb;
    explicit SuspendGuard(std::function<void(bool)> cb_) : cb(std::move(cb_)) {
        call_suspend_cb(cb, true);
    }
    ~SuspendGuard() { call_suspend_cb(cb, false); }
    SuspendGuard(const SuspendGuard&) = delete;
    SuspendGuard& operator=(const SuspendGuard&) = delete;
};

void FirmwareUpdater::runJob(const FlashJob& job) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        busy_ = true;
        last_error_.clear();
        log_.clear();
        state_ = FlashState::Idle;
    }

    logLine("[telemetry] Suspending telemetry reader to free serial port");
    SuspendGuard sg(suspend_cb_);

    logLine("==================================================");
    logLine(" Firmware update started");
    logLine(" File: " + job.firmware_path);
    logLine(" Port: " + job.port);
    logLine(" Mode: " + std::string(job.auto_gpio ? "AUTO (MCP2221A)" : "MANUAL"));
    logLine("==================================================");

    std::string cli;
    bool found = findCli(cli);
    if (!found) {
        last_error_ = "STM32_Programmer_CLI not found. Install STM32CubeCLT.";
        logLine("[ERROR] " + last_error_);
        setState(FlashState::Failed);
        busy_ = false;
        return;
    }
    logLine(std::string("[CLI] ") + cli);

    std::string gpio_helper;
    bool auto_mode = false;
    if (job.auto_gpio) {
        if (findGpioHelper(gpio_helper)) {
            logLine(std::string("[GPIO helper] ") + gpio_helper);
            auto_mode = true;
        } else {
            logLine("[WARN] MCP2221A GPIO helper not found; falling back to MANUAL mode.");
            logLine("       Place tools/mcp2221a_gpio.py next to the executable.");
        }
    }

    // 1. Drain serial before touching reset/BOOT0.
    drainSerial(job.port);

    // 2. Enter bootloader.
    setState(FlashState::EnteringBoot);
    if (auto_mode) {
        if (!gpioCommand(gpio_helper, "enter")) {
            logLine("[WARN] GPIO enter failed; continuing in MANUAL mode.");
            auto_mode = false;
        }
    } else {
        logLine("[MANUAL] 1. Hold BOOT0 HIGH");
        logLine("[MANUAL] 2. Press and release RESET");
        logLine("[MANUAL] Waiting 5 seconds for user...");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    // Drain again now that the ROM bootloader is running: the application may
    // have left kilobytes of telemetry buffered in the USB-UART bridge, which
    // would otherwise drown the bootloader's sync ACK.
    drainSerial(job.port);

    // 3. Flash + verify.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    setState(FlashState::Flashing);

    std::ostringstream cmd;
    cmd << "\"" << cli << "\" -c port=" << job.port << " br=" << FLASH_BAUDRATE
        << " -w \"" << job.firmware_path << "\" " << FLASH_ADDR << " -v";

    if (!runCommand(cmd.str(), "Flash")) {
        setState(FlashState::Failed);
        if (auto_mode) gpioCommand(gpio_helper, "release");
        busy_ = false;
        return;
    }

    setState(FlashState::Verifying);
    logLine("[verify] Download verified successfully");

    // 4. Exit bootloader.
    setState(FlashState::ExitingBoot);
    if (auto_mode) {
        gpioCommand(gpio_helper, "exit");
        gpioCommand(gpio_helper, "release");
    } else {
        logLine("[MANUAL] 1. Release BOOT0 (pull LOW)");
        logLine("[MANUAL] 2. Press RESET to run application");
    }

    setState(FlashState::Done);
    logLine("Firmware update complete.");
    busy_ = false;

    if (job.delete_after_flash) {
        std::error_code ec;
        fs::remove(job.firmware_path, ec);
    }
}

void FirmwareUpdater::threadMain() {
    while (run_.load()) {
        FlashJob job;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&] { return !run_.load() || !queue_.empty(); });
            if (!run_.load()) break;
            if (queue_.empty()) continue;
            job = queue_.front();
            queue_.pop_front();
        }
        runJob(job);
    }
}
