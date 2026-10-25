#pragma once

// VARA host protocol on top of the ARQ engine (design proposal Sections 3.1 and 7.8).
// Turns host command lines into arq::Link calls, and Link events, PTT and received
// frames into the replies a VARA host (Pat, LinBPQ, Winlink Express) expects.
// No sockets and no audio: the TNC supplies both through the Radio and Host callbacks,
// and every call happens on one thread with the current time passed in.

#include <cctype>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "app_info.hh"
#include "arq/arq_session.hh"

class VaraHost {
public:
    struct Radio {
        // Frames to send back-to-back in one keyed transmission, all in mode `mode`
        std::function<void(const std::vector<std::vector<uint8_t>>& frames,
                           const arq::ModeDesc& mode, bool contend)> transmit;
    };
    struct Host {
        std::function<void(const std::string& line)> send_line;   // without the CR
        std::function<void(const std::vector<uint8_t>& data)> send_data;
        std::function<void(const std::string& msg)> log;
    };

    static constexpr const char* VERSION = APP_NAME " VARA " APP_VERSION;
    static constexpr int ALIVE_INTERVAL_MS = 60000;

    VaraHost(arq::Params params, Radio radio, Host host, uint32_t seed = std::random_device{}())
        : params_(params), radio_(std::move(radio)), host_(std::move(host)), link_(params, seed) {
        link_.on_transmit = [this](const arq::TxRequest& r) {
            if (radio_.transmit) radio_.transmit(r.frames, r.mode, r.when == arq::TxWhen::CONTEND);
        };
        link_.on_rx_data = [this](const std::vector<uint8_t>& d) {
            if (host_.send_data) host_.send_data(d);
        };
        link_.on_host_event = [this](const arq::HostEvent& e) { host_event(e); };
        link_.on_log = [this](const std::string& m) { log("ARQ: " + m); };
    }

    // ---- host side ----

    void command(const std::string& raw, int64_t now) {
        std::istringstream in(raw);
        std::vector<std::string> w;
        for (std::string t; in >> t;) w.push_back(t);
        if (w.empty()) return;
        std::string cmd = upper(w[0]);
        auto arg = [&](size_t i) { return i < w.size() ? upper(w[i]) : std::string(); };

        if (cmd == "MYCALL" && w.size() >= 2) {
            // VARA takes several calls; the link answers to the first
            link_.set_mycall(arg(1));
            mycall_ = arg(1);
            reply("OK");
        } else if (cmd == "LISTEN") {
            std::string a = arg(1);
            if (a == "ON" || a == "OFF") {
                link_.listen(a == "ON");
                reply("OK");
            } else if (a == "CQ") {
                reply("OK");   // CQ frames are not supported; accept so hosts carry on
            } else {
                reply("WRONG");
            }
        } else if (cmd == "CONNECT" && w.size() >= 3) {
            // CONNECT <mycall> <target> [VIA ...]: the session uses the call given here
            link_.set_mycall(arg(1));
            mycall_ = arg(1);
            if (w.size() > 3) log("VARA: digipeater path ignored (not supported)");
            if (link_.connect(arg(2), now)) {
                reply("OK");
            } else {
                reply("WRONG");
            }
        } else if (cmd == "DISCONNECT") {
            reply("OK");
            if (link_.state() == arq::State::DISCONNECTED)
                reply("DISCONNECTED");
            else
                link_.disconnect(now);
        } else if (cmd == "ABORT" || cmd == "DIRTYDISCONNECT") {
            reply("OK");
            if (link_.state() == arq::State::DISCONNECTED)
                reply("DISCONNECTED");
            else
                link_.abort(now);
        } else if (cmd.rfind("BW", 0) == 0) {
            // BW500 / BW2300 / BW2750 (VARA HF), BW NARROW / BW WIDE, BWWIDE.
            // Only the narrow ladder exists so far, so every setting offers narrow.
            reply("OK");
        } else if (cmd == "VERSION") {
            reply(std::string("VERSION ") + VERSION);
        } else if (cmd == "COMPRESSION" || cmd == "CWID" || cmd == "CHAT" || cmd == "PUBLIC" ||
                   cmd == "ENCRYPTION" || cmd == "WINLINK" || cmd == "P2P" || cmd == "CLEANTXBUFFER") {
            reply("OK");   // accepted and ignored
        } else {
            log("VARA: unknown command '" + raw + "'");
            reply("WRONG");
        }
    }

    void data(const std::vector<uint8_t>& d, int64_t now) {
        if (link_.state() != arq::State::CONNECTED) {
            log("VARA: " + std::to_string(d.size()) + " data bytes while not connected, discarded");
            return;
        }
        link_.send(d, now);
    }

    void host_connected(int64_t now) {
        host_up_ = true;
        last_alive_ = now;
    }

    // The host closed its command connection: end any session at once
    void host_gone(int64_t now) {
        host_up_ = false;
        if (link_.state() != arq::State::DISCONNECTED) link_.abort(now);
    }

    // ---- radio side ----

    void frame_received(const std::vector<uint8_t>& bytes, float snr, int air_ms, int64_t now) {
        arq::State before = link_.state();
        link_.frame_received(bytes, snr, air_ms, now);
        if (before == arq::State::CONNECTED || link_.state() == arq::State::CONNECTED) {
            char buf[32];
            snprintf(buf, sizeof buf, "SN %.1f", snr);
            reply(buf);
        }
    }

    // Channel busy (a frame is being received): holds the engine's sends and timeouts,
    // and is reported to the host as BUSY ON / BUSY OFF
    void channel_busy(bool busy) {
        link_.channel_busy(busy);
        if (busy != busy_) {
            busy_ = busy;
            reply(busy ? "BUSY ON" : "BUSY OFF");
        }
    }

    void keyed(int64_t now) {
        reply("PTT ON");
        link_.tx_event(arq::TxEventType::KEYED, now);
        // the sending side reports the step it is sending at
        if (link_.is_iss()) report_bitrate();
    }

    void unkeyed(int64_t now) {
        reply("PTT OFF");
        link_.tx_event(arq::TxEventType::UNKEYED, now);
        // the receiving side reports the step its ACK just asked for
        if (!link_.is_iss()) report_bitrate();
    }

    void tick(int64_t now) {
        link_.tick(now);
        if (host_up_ && now - last_alive_ >= ALIVE_INTERVAL_MS) {
            last_alive_ = now;
            reply("IAMALIVE");
        }
    }

    const arq::Link& link() const { return link_; }

private:
    void host_event(const arq::HostEvent& e) {
        switch (e.type) {
        case arq::HostEventType::PENDING:
            reply("PENDING");
            break;
        case arq::HostEventType::CANCEL_PENDING:
            reply("CANCELPENDING");
            break;
        case arq::HostEventType::CONNECTED: {
            // VARA names the calling station first
            const std::string& caller = e.outgoing ? e.local : e.remote;
            const std::string& called = e.outgoing ? e.remote : e.local;
            reply("CONNECTED " + caller + " " + called + (e.bw == arq::BW_WIDE ? " WIDE" : " NARROW"));
            last_step_ = -1;
            break;
        }
        case arq::HostEventType::DISCONNECTED:
            log("VARA: disconnected (" + e.reason + ")");
            reply("DISCONNECTED");
            break;
        case arq::HostEventType::BUFFER:
            reply("BUFFER " + std::to_string(e.buffer));
            break;
        }
    }

    // BITRATE (level) bps, sent when the speed level changes. The receiving side only knows
    // a step once it has measured a burst, so each side reports what it controls.
    void report_bitrate() {
        if (link_.state() != arq::State::CONNECTED) return;
        int step = link_.is_iss() ? link_.tx_step() : link_.rx_step();
        const auto& ladder = params_.ladder(link_.bandwidth());
        if (step < 0 || step >= (int)ladder.size() || step == last_step_) return;
        last_step_ = step;
        const arq::ModeDesc& m = ladder[step].mode;
        int bps = m.air_ms > 0 ? (int)((int64_t)m.payload * 8 * 1000 / m.air_ms) : 0;
        reply("BITRATE (" + std::to_string(step + 1) + ") " + std::to_string(bps) + " bps");
    }

    void reply(const std::string& line) {
        if (host_.send_line) host_.send_line(line);
    }

    void log(const std::string& msg) {
        if (host_.log) host_.log(msg);
    }

    static std::string upper(std::string s) {
        for (auto& c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    }

    arq::Params params_;
    Radio radio_;
    Host host_;
    arq::Link link_;
    std::string mycall_;
    bool host_up_ = false;
    bool busy_ = false;
    int64_t last_alive_ = 0;
    int last_step_ = -1;
};
