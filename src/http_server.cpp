#include "http_server.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
static const socket_t INVALID_SOCKET_VAL = INVALID_SOCKET;
static bool g_wsa_init = false;
static void ensure_wsa() {
    if (!g_wsa_init) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        g_wsa_init = true;
    }
}
#define CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int socket_t;
static const socket_t INVALID_SOCKET_VAL = -1;
#define CLOSE_SOCKET close
#endif

static bool set_nonblocking(socket_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
}

static std::string currentThreadIdStr() {
    std::ostringstream oss;
    oss << std::this_thread::get_id();
    return oss.str();
}

static std::string makeTempFwPath() {
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec);
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    std::string name = "rte_fw_" + std::to_string(ms) + "_" + currentThreadIdStr() + ".bin";
    return (tmp / name).string();
}

static void sendResponse(socket_t client, int code, const std::string& body) {
    const char* status = "OK";
    if (code == 202) status = "Accepted";
    else if (code == 400) status = "Bad Request";
    else if (code == 405) status = "Method Not Allowed";
    else if (code == 503) status = "Service Unavailable";
    else if (code == 500) status = "Internal Server Error";

    std::ostringstream resp;
    resp << "HTTP/1.1 " << code << " " << status << "\r\n";
    resp << "Content-Type: application/json\r\n";
    resp << "Content-Length: " << body.size() << "\r\n";
    resp << "Connection: close\r\n";
    resp << "\r\n";
    resp << body;

    std::string s = resp.str();
#ifdef _WIN32
    send(client, s.c_str(), (int)s.size(), 0);
#else
    send(client, s.c_str(), s.size(), MSG_NOSIGNAL);
#endif
}

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    out += esc;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

HttpFlashServer::HttpFlashServer(FirmwareUpdater& updater, std::string port)
    : updater_(updater), port_(std::move(port)) {}

HttpFlashServer::~HttpFlashServer() {
    stop();
}

bool HttpFlashServer::start() {
    return start(port_);
}

bool HttpFlashServer::start(const std::string& port) {
#ifdef _WIN32
    ensure_wsa();
#endif
    if (running_.load()) return true;

    port_ = port;
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET_VAL) return false;

    int opt = 1;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    if (!set_nonblocking(fd)) {
        CLOSE_SOCKET(fd);
        return false;
    }

    int p = 0;
    try {
        p = std::stoi(port_);
    } catch (...) {
        CLOSE_SOCKET(fd);
        return false;
    }
    if (p <= 0 || p > 65535) {
        CLOSE_SOCKET(fd);
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)p);

    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        CLOSE_SOCKET(fd);
        return false;
    }

    if (listen(fd, 4) != 0) {
        CLOSE_SOCKET(fd);
        return false;
    }

    // Read back actual port in case port_ was "0".
    socklen_t addrlen = sizeof(addr);
    if (getsockname(fd, (sockaddr*)&addr, &addrlen) == 0) {
        actual_port_.store((int)ntohs(addr.sin_port));
    } else {
        actual_port_.store(p);
    }

    listen_fd_ = fd;
    stop_.store(false);
    running_.store(true);
    thread_ = std::thread(&HttpFlashServer::threadMain, this);
    return true;
}

void HttpFlashServer::stop() {
    stop_.store(true);
    if (listen_fd_ != INVALID_SOCKET_VAL) {
        CLOSE_SOCKET(listen_fd_);
        listen_fd_ = INVALID_SOCKET_VAL;
    }
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

bool HttpFlashServer::restart(const std::string& port) {
    stop();
    return start(port);
}

void HttpFlashServer::threadMain() {
    while (!stop_.load()) {
        sockaddr_in client_addr{};
        socklen_t addrlen = sizeof(client_addr);
        socket_t client = accept(listen_fd_, (sockaddr*)&client_addr, &addrlen);
        if (client == INVALID_SOCKET_VAL) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
#endif
            continue;
        }

        // Set receive timeout so a misbehaving client cannot hang us forever.
#ifdef _WIN32
        DWORD recv_timeout = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recv_timeout, sizeof(recv_timeout));
#else
        timeval tv{};
        tv.tv_sec = 5;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        // Read request into buffer.
        std::string buf;
        buf.reserve(4096);
        char tmp[4096];
        bool header_complete = false;
        size_t header_end = 0;
        while (!stop_.load()) {
            int n = recv(client, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            buf.append(tmp, (size_t)n);

            if (!header_complete) {
                header_end = buf.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    header_complete = true;
                    header_end += 4;
                }
            }

            if (header_complete) break;
        }

        if (!header_complete) {
            sendResponse(client, 400, "{\"error\":\"incomplete request\"}");
            CLOSE_SOCKET(client);
            continue;
        }

        // Parse request line.
        size_t line_end = buf.find("\r\n");
        std::string request_line = buf.substr(0, line_end);
        std::istringstream iss(request_line);
        std::string method, path, version;
        iss >> method >> path >> version;

        if (method == "GET" && path == "/flash/status") {
            FlashStatus st = updater_.status();
            std::string resp = std::string("{\"state\":\"")
                + FirmwareUpdater::stateString(st.state)
                + "\",\"busy\":" + (st.busy ? "true" : "false")
                + ",\"last_error\":\"" + jsonEscape(st.last_error) + "\"";
            // Include the tail of the updater log for remote debugging.
            const size_t kLogTail = 30;
            size_t start = st.log.size() > kLogTail ? st.log.size() - kLogTail : 0;
            resp += ",\"log\":[";
            for (size_t i = start; i < st.log.size(); ++i) {
                resp += (i == start ? "\"" : ",\"") + jsonEscape(st.log[i]) + "\"";
            }
            resp += "]}";
            sendResponse(client, 200, resp);
            CLOSE_SOCKET(client);
            continue;
        }

        if (method != "POST" || path != "/flash") {
            sendResponse(client, 405, "{\"error\":\"only POST /flash and GET /flash/status are supported\"}");
            CLOSE_SOCKET(client);
            continue;
        }

        // Parse headers.
        size_t pos = line_end + 2;
        size_t content_length = 0;
        bool has_content_length = false;
        while (pos < header_end - 2) {
            size_t next = buf.find("\r\n", pos);
            if (next == std::string::npos) break;
            std::string header = buf.substr(pos, next - pos);
            size_t colon = header.find(':');
            if (colon != std::string::npos) {
                std::string key = header.substr(0, colon);
                std::string value = header.substr(colon + 1);
                // trim
                size_t a = value.find_first_not_of(" \t\r\n");
                size_t b = value.find_last_not_of(" \t\r\n");
                if (a != std::string::npos) value = value.substr(a, b - a + 1);
                std::transform(key.begin(), key.end(), key.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (key == "content-length") {
                    content_length = std::stoull(value);
                    has_content_length = true;
                }
            }
            pos = next + 2;
        }

        if (!has_content_length) {
            sendResponse(client, 400, "{\"error\":\"missing Content-Length\"}");
            CLOSE_SOCKET(client);
            continue;
        }

        // Read remaining body bytes.
        std::string body;
        body.reserve(content_length);
        size_t have = buf.size() - header_end;
        if (have > 0) body.append(buf.data() + header_end, have);

        while (body.size() < content_length && !stop_.load()) {
            int to_read = (int)std::min<size_t>(sizeof(tmp), content_length - body.size());
            int n = recv(client, tmp, to_read, 0);
            if (n <= 0) break;
            body.append(tmp, (size_t)n);
        }

        if (body.size() != content_length) {
            sendResponse(client, 400, "{\"error\":\"short body\"}");
            CLOSE_SOCKET(client);
            continue;
        }

        // Save to temp file.
        std::string tmp_path = makeTempFwPath();
        {
            std::ofstream out(tmp_path, std::ios::binary);
            if (!out) {
                sendResponse(client, 500, "{\"error\":\"cannot write temp file\"}");
                CLOSE_SOCKET(client);
                continue;
            }
            out.write(body.data(), (std::streamsize)body.size());
            out.close();
        }

        // Queue flash job using the telemetry port registered with the updater.
        FlashJob job;
        job.firmware_path = tmp_path;
        job.port = updater_.currentPort();
        job.auto_gpio = true; // default for HTTP; could be exposed later
        job.delete_after_flash = true;

        if (job.port.empty()) {
            fs::remove(tmp_path);
            sendResponse(client, 503, "{\"error\":\"telemetry port not known yet\"}");
            CLOSE_SOCKET(client);
            continue;
        }

        if (!updater_.queueFlash(job, false)) {
            fs::remove(tmp_path);
            sendResponse(client, 503, "{\"error\":\"already flashing\"}");
            CLOSE_SOCKET(client);
            continue;
        }

        std::string resp = "{\"queued\":true,\"file\":\"" + jsonEscape(tmp_path) + "\"}";
        sendResponse(client, 202, resp);
        CLOSE_SOCKET(client);
    }

    running_.store(false);
}
