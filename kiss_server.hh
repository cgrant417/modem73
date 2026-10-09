#pragma once

#include <iostream>
#include <cstring>
#include <cerrno>
#include <vector>
#include <list>
#include <mutex>
#include <memory>
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

#include "kiss_tnc.hh"


class ClientConnection {
public:
    int fd;
    KISSParser parser;
    std::vector<uint8_t> write_buffer;
    std::mutex write_mutex;
    bool connected = true;

    ClientConnection(int fd, std::function<void(uint8_t, uint8_t, const std::vector<uint8_t>&)> callback)
        : fd(fd), parser(callback) {}

    static constexpr size_t MAX_WRITE_BUFFER = 1024 * 1024;

    void send(const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lock(write_mutex);
        if (write_buffer.size() + data.size() > MAX_WRITE_BUFFER)
            return;
        write_buffer.insert(write_buffer.end(), data.begin(), data.end());
    }

    bool flush() {
        std::lock_guard<std::mutex> lock(write_mutex);
        if (write_buffer.empty()) return true;

        ssize_t sent = ::send(fd, write_buffer.data(), write_buffer.size(), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            return false;
        }
        write_buffer.erase(write_buffer.begin(), write_buffer.begin() + sent);
        return true;
    }
};


// KISS TCP server: accepts KISS clients, hands their data frames to the TNC
// and sends received frames back to every client. Polled from the TNC main loop.
class KISSServer {
public:
    struct TNCInterface {
        std::function<void(const std::vector<uint8_t>&)> submit_frame;  // KISS data frame to transmit
        std::function<void(const std::string&)> log;
        std::function<void(size_t)> client_count_changed;
    };

    KISSServer(TNCInterface iface, bool verbose = false)
        : iface_(std::move(iface)), verbose_(verbose) {}

    ~KISSServer() {
        close_all();
    }

    void open(const std::string& bind_address, int port) {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            throw std::runtime_error("Failed to create socket");
        }

        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        if (inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1) {
            close(server_fd_);
            server_fd_ = -1;
            throw std::runtime_error("Invalid bind address: " + bind_address);
        }
        addr.sin_port = htons(port);

        if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(server_fd_);
            server_fd_ = -1;
            throw std::runtime_error("Failed to bind to port " + std::to_string(port));
        }

        if (listen(server_fd_, 5) < 0) {
            close(server_fd_);
            server_fd_ = -1;
            throw std::runtime_error("Failed to listen");
        }

        fcntl(server_fd_, F_SETFL, O_NONBLOCK);

        std::cerr << "KISS TNC listening on " << bind_address << ":" << port << std::endl;
    }

    // Accept new clients, read their frames and flush pending output
    void poll() {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd >= 0) {
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                if (clients_.size() >= MAX_CLIENTS) {
                    log("KISS: client limit reached, rejecting connection");
                    close(client_fd);
                    client_fd = -1;
                }
            }
        }

        if (client_fd >= 0) {
            // Set TCP_NODELAY
            int flag = 1;
            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
            fcntl(client_fd, F_SETFL, O_NONBLOCK);

            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
            log(std::string("Client connected: ") + ip_str + ":" + std::to_string(ntohs(client_addr.sin_port)));

            auto callback = [this](uint8_t port, uint8_t cmd, const std::vector<uint8_t>& data) {
                handle_kiss_frame(port, cmd, data);
            };

            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients_.emplace_back(std::make_unique<ClientConnection>(client_fd, callback));
            client_count_changed();
        }

        // Poll clients for data
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (auto it = clients_.begin(); it != clients_.end();) {
                auto& client = *it;

                // Read data
                uint8_t buf[4096];
                ssize_t n = recv(client->fd, buf, sizeof(buf), MSG_DONTWAIT);

                if (n > 0) {
                    client->parser.process(buf, n);
                } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    // Disconnected
                    log("Client disconnected");
                    close(client->fd);
                    it = clients_.erase(it);
                    client_count_changed();
                    continue;
                }

                // Flush write buffer
                if (!client->flush()) {
                    log("Client write error, disconnecting");
                    close(client->fd);
                    it = clients_.erase(it);
                    client_count_changed();
                    continue;
                }

                ++it;
            }
        }
    }

    // Queue a received frame for every connected client
    void send_frame(const std::vector<uint8_t>& payload) {
        auto kiss_frame = KISSParser::wrap(payload);

        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& client : clients_) {
            client->send(kiss_frame);
        }
    }

    size_t client_count() const {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        return clients_.size();
    }

    void close_all() {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& client : clients_) {
            close(client->fd);
        }
        clients_.clear();
        if (server_fd_ >= 0) {
            close(server_fd_);
            server_fd_ = -1;
        }
    }

private:
    void handle_kiss_frame(uint8_t /*port*/, uint8_t cmd, const std::vector<uint8_t>& data) {
        if (cmd == KISS::CMD_DATA) {
            if (verbose_) {
                std::cerr << kiss_frame_visualize(data.data(), data.size()) << std::endl;
            }
            if (iface_.submit_frame) iface_.submit_frame(data);
        } else {
            switch (cmd) {
                // unused handled by modem73 config
            case KISS::CMD_TXDELAY:
                if (!data.empty()) {
                    //
                }
                break;
            case KISS::CMD_P:
                if (!data.empty()) {
                    //
                }
                break;
            case KISS::CMD_SLOTTIME:
                if (!data.empty()) {
                    //
                }
                break;
            case KISS::CMD_TXTAIL:
                if (!data.empty()) {
                    //
                }
                break;
            case KISS::CMD_FULLDUPLEX:
                if (!data.empty()) {
                    log("KISS full duplex request ignored");
                }
                break;
            case KISS::CMD_SETHW:
                break;
            case KISS::CMD_RETURN:
                break;
            default:
                if (verbose_) {
                    std::cerr << "Unknown KISS command: 0x" << std::hex << (int)cmd << std::dec << std::endl;
                }
            }
        }
    }

    void log(const std::string& msg) {
        if (iface_.log) iface_.log(msg);
    }

    // Called with clients_mutex_ held
    void client_count_changed() {
        if (iface_.client_count_changed) iface_.client_count_changed(clients_.size());
    }

    static constexpr size_t MAX_CLIENTS = 16;
    TNCInterface iface_;
    bool verbose_;
    int server_fd_ = -1;
    std::list<std::unique_ptr<ClientConnection>> clients_;
    mutable std::mutex clients_mutex_;
};
