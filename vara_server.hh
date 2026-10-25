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
// Polled from the TNC main loop. Commands and data are handed to the TNC after the
// socket lock is released, so its handlers may call send_line() / send_data().
class VARAServer {
public:
    struct TNCInterface {
        std::function<void(const std::string&)> log;
        std::function<void(size_t)> client_count_changed;
        std::function<void(const std::string& line)> command;          // one line, without CR/LF
        std::function<void(const std::vector<uint8_t>&)> data;          // bytes from the host
        std::function<void(bool connected)> host_changed;               // command port host came or went
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
    }

    void poll() {
        std::vector<std::string> lines;
        std::vector<uint8_t> data;
        std::vector<bool> host_changes;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (accept_one(cmd_, "command")) host_changes.push_back(true);
            accept_one(data_, "data");

            uint8_t buf[4096];
            ssize_t n;
            bool gone = false;
            while (cmd_.fd >= 0 && (n = read_some(cmd_, buf, sizeof(buf), "command", gone)) > 0) {
                cmd_buf_.append(reinterpret_cast<char*>(buf), n);
                size_t eol;
                while ((eol = cmd_buf_.find_first_of("\r\n")) != std::string::npos) {
                    std::string line = cmd_buf_.substr(0, eol);
                    cmd_buf_.erase(0, eol + 1);
                    if (!line.empty())
                        lines.push_back(line);
                }
                if (cmd_buf_.size() > MAX_CMD_LINE) {
                    log("VARA: command line too long, discarded");
                    cmd_buf_.clear();
                }
            }
            if (gone) host_changes.push_back(false);
            bool data_gone = false;
            while (data_.fd >= 0 && (n = read_some(data_, buf, sizeof(buf), "data", data_gone)) > 0)
                data.insert(data.end(), buf, buf + n);
            flush_pending(cmd_, cmd_out_);
            flush_pending(data_, data_out_);
            if (pending_host_gone_) {   // a failed write dropped the command host
                pending_host_gone_ = false;
                host_changes.push_back(false);
            }
        }
        for (bool up : host_changes)
            if (iface_.host_changed) iface_.host_changed(up);
        for (const auto& line : lines) {
            if (verbose) log("VARA: host> " + line);
            if (iface_.command) iface_.command(line);
        }
        if (!data.empty() && iface_.data) iface_.data(data);
    }

    // A reply or notification to the command-port host (CR is added)
    void send_line(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (verbose) log("VARA: host< " + line);
        if (cmd_.fd < 0) return;
        cmd_out_.insert(cmd_out_.end(), line.begin(), line.end());
        cmd_out_.push_back('\r');
        flush_pending(cmd_, cmd_out_);
    }

    // Received payload bytes for the data-port host
    void send_data(const std::vector<uint8_t>& bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (data_.fd < 0) {
            log("VARA: no host on the data port, " + std::to_string(bytes.size()) + " bytes dropped");
            return;
        }
        data_out_.insert(data_out_.end(), bytes.begin(), bytes.end());
        flush_pending(data_, data_out_);
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
        cmd_out_.clear();
        data_out_.clear();
    }

    bool verbose = false;   // log every command and reply

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

    // Called with mutex_ held. Returns true if a host was accepted.
    bool accept_one(Socket& s, const char* name) {
        if (s.listen_fd < 0) return false;
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int fd = accept(s.listen_fd, (struct sockaddr*)&addr, &len);
        if (fd < 0) return false;

        if (s.fd >= 0) {
            log(std::string("VARA: ") + name + " port already has a host, rejecting connection");
            close(fd);
            return false;
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
        return true;
    }

    // Called with mutex_ held. Returns bytes read, 0 if nothing pending or the host left
    // (then `gone` is set).
    ssize_t read_some(Socket& s, uint8_t* buf, size_t len, const char* name, bool& gone) {
        ssize_t n = recv(s.fd, buf, len, MSG_DONTWAIT);
        if (n > 0) return n;
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            drop(s, name, gone);
        return 0;
    }

    // Called with mutex_ held: write what the socket takes now, keep the rest
    void flush_pending(Socket& s, std::vector<uint8_t>& out) {
        while (s.fd >= 0 && !out.empty()) {
            ssize_t n = ::send(s.fd, out.data(), out.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n > 0) {
                out.erase(out.begin(), out.begin() + n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            bool gone = false;
            drop(s, &s == &cmd_ ? "command" : "data", gone);
            if (gone) pending_host_gone_ = true;
        }
        if (s.fd < 0) out.clear();
    }

    // Called with mutex_ held
    void drop(Socket& s, const char* name, bool& gone) {
        log(std::string("VARA: host disconnected from ") + name + " port");
        close(s.fd);
        s.fd = -1;
        if (&s == &cmd_) {
            cmd_buf_.clear();
            cmd_out_.clear();
            client_count_changed();
            gone = true;
        } else {
            data_out_.clear();
        }
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
    std::vector<uint8_t> cmd_out_;
    std::vector<uint8_t> data_out_;
    bool pending_host_gone_ = false;
    mutable std::mutex mutex_;
};
