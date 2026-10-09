#pragma once

#include <iostream>
#include <cstring>
#include <cerrno>
#include <vector>
#include <mutex>
#include <string>
#include <functional>
#include <stdexcept>

// Network
#include <sys/socket.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>


// VARA-compatible host interface: an ASCII command socket on vara_port and a
// byte-stream data socket on vara_port + 1, each serving a single host (as VARA does).
// Polled from the TNC main loop.
//
// Phase 3 (startup selection): listens and logs what the host sends. Commands are
// not answered and data is not transmitted yet; that arrives with the ARQ engine.
class VARAServer {
public:
    struct TNCInterface {
        std::function<void(const std::string&)> log;
        std::function<void(size_t)> client_count_changed;
    };

    explicit VARAServer(TNCInterface iface) : iface_(std::move(iface)) {}

    ~VARAServer() {
        close_all();
    }

    void open(const std::string& bind_address, int port) {
        cmd_.listen_fd = listen_on(bind_address, port);
        try {
            data_.listen_fd = listen_on(bind_address, port + 1);
        } catch (...) {
            close(cmd_.listen_fd);
            cmd_.listen_fd = -1;
            throw;
        }
        std::cerr << "VARA host interface listening on " << bind_address << ":" << port
                  << " (command) and " << bind_address << ":" << port + 1 << " (data)" << std::endl;
        std::cerr << "(!) VARA host interface: commands are logged but not yet handled" << std::endl;
    }

    void poll() {
        std::lock_guard<std::mutex> lock(mutex_);
        accept_one(cmd_, "command");
        accept_one(data_, "data");

        uint8_t buf[4096];
        ssize_t n;
        while (cmd_.fd >= 0 && (n = read_some(cmd_, buf, sizeof(buf), "command")) > 0) {
            cmd_buf_.append(reinterpret_cast<char*>(buf), n);
            size_t eol;
            while ((eol = cmd_buf_.find_first_of("\r\n")) != std::string::npos) {
                std::string line = cmd_buf_.substr(0, eol);
                cmd_buf_.erase(0, eol + 1);
                if (!line.empty())
                    log("VARA: host command '" + line + "' (not handled yet)");
            }
            if (cmd_buf_.size() > MAX_CMD_LINE) {
                log("VARA: command line too long, discarded");
                cmd_buf_.clear();
            }
        }
        while (data_.fd >= 0 && (n = read_some(data_, buf, sizeof(buf), "data")) > 0)
            log("VARA: " + std::to_string(n) + " data bytes from host (not transmitted yet)");
    }

    size_t client_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cmd_.fd >= 0 ? 1 : 0;
    }

    void close_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Socket* s : {&cmd_, &data_}) {
            if (s->fd >= 0) { close(s->fd); s->fd = -1; }
            if (s->listen_fd >= 0) { close(s->listen_fd); s->listen_fd = -1; }
        }
        cmd_buf_.clear();
    }

private:
    struct Socket {
        int listen_fd = -1;
        int fd = -1;        // the connected host, if any
    };

    static int listen_on(const std::string& bind_address, int port) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            throw std::runtime_error("Failed to create socket");

        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        if (inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1) {
            close(fd);
            throw std::runtime_error("Invalid VARA bind address: " + bind_address);
        }
        addr.sin_port = htons(port);

        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd);
            throw std::runtime_error("Failed to bind to VARA port " + std::to_string(port));
        }
        if (listen(fd, 1) < 0) {
            close(fd);
            throw std::runtime_error("Failed to listen on VARA port " + std::to_string(port));
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);
        return fd;
    }

    // Called with mutex_ held
    void accept_one(Socket& s, const char* name) {
        if (s.listen_fd < 0) return;
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int fd = accept(s.listen_fd, (struct sockaddr*)&addr, &len);
        if (fd < 0) return;

        if (s.fd >= 0) {
            log(std::string("VARA: ") + name + " port already has a host, rejecting connection");
            close(fd);
            return;
        }
        int flag = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        fcntl(fd, F_SETFL, O_NONBLOCK);
        s.fd = fd;

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));
        log(std::string("VARA: host connected to ") + name + " port from " + ip_str + ":" +
            std::to_string(ntohs(addr.sin_port)));
        if (&s == &cmd_) client_count_changed();
    }

    // Called with mutex_ held. Returns bytes read, 0 if nothing pending or the host left.
    ssize_t read_some(Socket& s, uint8_t* buf, size_t len, const char* name) {
        ssize_t n = recv(s.fd, buf, len, MSG_DONTWAIT);
        if (n > 0) return n;
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            log(std::string("VARA: host disconnected from ") + name + " port");
            close(s.fd);
            s.fd = -1;
            if (&s == &cmd_) {
                cmd_buf_.clear();
                client_count_changed();
            }
        }
        return 0;
    }

    void log(const std::string& msg) {
        if (iface_.log) iface_.log(msg);
    }

    // Called with mutex_ held
    void client_count_changed() {
        if (iface_.client_count_changed) iface_.client_count_changed(cmd_.fd >= 0 ? 1 : 0);
    }

    static constexpr size_t MAX_CMD_LINE = 1024;
    TNCInterface iface_;
    Socket cmd_;
    Socket data_;
    std::string cmd_buf_;
    mutable std::mutex mutex_;
};
