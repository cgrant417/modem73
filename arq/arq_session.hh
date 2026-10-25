#pragma once

// ARQ link state machine (Section 8.6). Single-threaded and clock-free: every call
// passes the current time in milliseconds, so tests are deterministic. No sockets,
// no audio: frames go out through `on_transmit`, come in through frame_received(),
// and the radio reports key-up/down through tx_event().

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "arq_frames.hh"
#include "arq_params.hh"

namespace arq {

enum class TxWhen {
    IMMEDIATE,   // in-session reply: the engine has already waited the turnaround guard
    CONTEND,     // may be delayed by channel access (CONNECT, turn request)
};

struct TxRequest {
    std::vector<std::vector<uint8_t>> frames;   // sent back-to-back in one transmission
    ModeDesc mode;
    TxWhen when = TxWhen::IMMEDIATE;
};

enum class TxEventType { KEYED, UNKEYED };

enum class HostEventType { PENDING, CANCEL_PENDING, CONNECTED, DISCONNECTED, BUFFER };

struct HostEvent {
    explicit HostEvent(HostEventType t) : type(t) {}
    HostEventType type;
    std::string local, remote;   // CONNECTED
    bool outgoing = false;       // CONNECTED: we placed the call
    uint8_t bw = 0;              // CONNECTED: bandwidth class (BW_NARROW / BW_WIDE)
    std::string reason;          // DISCONNECTED
    size_t buffer = 0;           // BUFFER: bytes sent by the host and not yet acknowledged
};

enum class State { DISCONNECTED, CALLING, ACCEPTING, CONNECTED, DISCONNECTING };

inline const char* state_name(State s) {
    switch (s) {
    case State::DISCONNECTED: return "DISCONNECTED";
    case State::CALLING: return "CALLING";
    case State::ACCEPTING: return "ACCEPTING";
    case State::CONNECTED: return "CONNECTED";
    case State::DISCONNECTING: return "DISCONNECTING";
    }
    return "?";
}

class Link {
public:
    std::function<void(const TxRequest&)> on_transmit;
    std::function<void(const HostEvent&)> on_host_event;
    std::function<void(const std::vector<uint8_t>&)> on_rx_data;
    std::function<void(const std::string&)> on_log;

    explicit Link(Params p = Params(), uint32_t seed = std::random_device{}())
        : p_(std::move(p)), rng_(seed) {}

    // ---- host side ----

    void set_mycall(const std::string& call) { mycall_ = upper(call); }
    void listen(bool on) { listen_ = on; }

    bool connect(const std::string& target, int64_t now) {
        if (state_ != State::DISCONNECTED || mycall_.empty()) return false;
        reset_session();
        session_ = random_session();
        peer_ = upper(target);
        outgoing_ = true;
        state_ = State::CALLING;
        connect_attempts_ = 0;
        schedule(Action::CONNECT, now);
        log("calling " + peer_);
        return true;
    }

    void send(const std::vector<uint8_t>& data, int64_t now) {
        if (data.empty()) return;
        tx_buf_.insert(tx_buf_.end(), data.begin(), data.end());
        report_buffer();
        if (state_ == State::CONNECTED && iss_ && wait_ == Wait::IDLE) {
            cancel(Action::POLL_KEEPALIVE);
            wait_ = Wait::NONE;
            schedule(Action::ISS_DECIDE, std::max(now, last_activity_ + p_.turnaround_ms));
        }
    }

    // Graceful: sends what is buffered, then disconnects
    void disconnect(int64_t now) {
        switch (state_) {
        case State::DISCONNECTED:
        case State::DISCONNECTING:
            return;
        case State::CALLING:
        case State::ACCEPTING:
            close("disconnected", now);
            return;
        case State::CONNECTED:
            disconnect_requested_ = true;
            if (iss_ && wait_ == Wait::IDLE) {
                cancel(Action::POLL_KEEPALIVE);
                wait_ = Wait::NONE;
                schedule(Action::ISS_DECIDE, now);
            }
            return;
        }
    }

    // Immediate: one DISC, no wait
    void abort(int64_t now) {
        if (state_ == State::DISCONNECTED) return;
        if (state_ == State::CONNECTED || state_ == State::DISCONNECTING || state_ == State::ACCEPTING) {
            // If the peer is about to answer us, let it finish so the DISC is not lost under it
            if (tx_busy_) {
                abort_disc_session_ = session_;   // scheduled from our unkeyed event
            } else {
                int64_t at = now;
                if (wait_ == Wait::ACK || wait_ == Wait::TURN_CONFIRM || wait_ == Wait::DISC_ACK)
                    at = std::max(now, last_activity_ + p_.ack_timeout_ms());
                schedule(Action::DISC_ONCE, at, session_, (uint8_t)DiscReason::ABORT);
            }
        }
        close("aborted", now);
    }

    // ---- radio side ----

    // A decoded PHY frame. air_ms is the airtime of that frame's mode (used to predict burst end).
    void frame_received(const std::vector<uint8_t>& bytes, float snr, int air_ms, int64_t now) {
        Frame f;
        if (!decode(bytes.data(), bytes.size(), f)) return;
        if (f.type == FrameType::CONNECT) {
            handle_connect(f, snr, now);
            return;
        }
        if (state_ == State::DISCONNECTED || f.session != session_) {
            if (f.type == FrameType::DISC && f.session == last_session_ && now < last_session_until_)
                schedule(Action::DISC_ACK, now + p_.turnaround_ms, f.session);
            return;
        }
        last_rx_ms_ = now;
        last_activity_ = now;
        turn_req_at_ = 0;
        peer_idle_ = false;
        retries_ = 0;
        last_snr_ = clamp_snr(snr);
        if (state_ == State::ACCEPTING && f.type != FrameType::CONNECT_ACK) promote_accepted(now);

        switch (f.type) {
        case FrameType::CONNECT_ACK: handle_connect_ack(f, now); break;
        case FrameType::REJECT:
            if (state_ == State::CALLING) close(std::string("rejected: ") + reject_name(f.reason), now);
            break;
        case FrameType::DATA: handle_data(f, snr, air_ms, now); break;
        case FrameType::ACK: handle_ack(f, now); break;
        case FrameType::POLL:
            peer_idle_ = (f.flags & POLL_KEEPALIVE) != 0;
            handle_poll(f, now);
            break;
        case FrameType::TURN: handle_turn(f, now); break;
        case FrameType::DISC:
            schedule(Action::DISC_ACK, now + p_.turnaround_ms, f.session);
            close("remote disconnected", now);
            break;
        case FrameType::DISC_ACK:
            if (state_ == State::DISCONNECTING) close("disconnected", now);
            break;
        case FrameType::CONNECT: break;
        }
    }

    void tx_event(TxEventType t, int64_t now) {
        if (t == TxEventType::KEYED) return;
        tx_busy_ = false;
        last_activity_ = now;
        turn_req_at_ = 0;
        if (abort_disc_session_) {
            schedule(Action::DISC_ONCE, now + p_.ack_timeout_ms(), abort_disc_session_, (uint8_t)DiscReason::ABORT);
            abort_disc_session_ = 0;
        }
        switch (last_tx_) {
        case Sent::CONNECT:
            if (state_ == State::CALLING)
                set_wait(Wait::CONNECT_REPLY, now + rand_between(p_.connect_retry_min_ms, p_.connect_retry_max_ms));
            break;
        case Sent::DATA:
        case Sent::POLL:
            if (state_ == State::CONNECTED && iss_) set_wait(Wait::ACK, now + p_.ack_timeout_ms());
            break;
        case Sent::POLL_KEEPALIVE:
            if (state_ == State::CONNECTED && iss_) set_wait(Wait::ACK, now + p_.ack_timeout_ms());
            break;
        case Sent::TURN:
            if (state_ == State::CONNECTED && !iss_) set_wait(Wait::TURN_CONFIRM, now + p_.turn_timeout_ms());
            break;
        case Sent::DISC:
            if (state_ == State::DISCONNECTING) set_wait(Wait::DISC_ACK, now + p_.ack_timeout_ms());
            break;
        case Sent::OTHER:
            break;
        }
        last_tx_ = Sent::OTHER;
    }

    // A frame is being received right now (the demodulator is synced). Scheduled sends and
    // reply timeouts wait until it ends: keying would destroy it, and it usually answers
    // what we were waiting for. The caller caps how long a stuck detector can hold this.
    void channel_busy(bool busy) { rx_busy_ = busy; }

    // ---- clock ----

    void tick(int64_t now) {
        // timeouts
        if (wait_ != Wait::NONE && wait_ != Wait::IDLE && now >= wait_until_ && !tx_busy_ && !rx_busy_) {
            Wait w = wait_;
            wait_ = Wait::NONE;
            on_wait_expired(w, now);
        }
        if ((state_ == State::CONNECTED || state_ == State::ACCEPTING || state_ == State::DISCONNECTING) &&
            now - last_rx_ms_ > p_.link_timeout_ms) {
            close("link failure: no frames from peer", now);
        }
        maybe_request_turn(now);
        // one scheduled transmission per call when the radio and the channel are free
        if (!tx_busy_ && !rx_busy_) {
            auto it = std::min_element(sched_.begin(), sched_.end(),
                                       [](const Sched& a, const Sched& b) { return a.at < b.at; });
            if (it != sched_.end() && it->at <= now) {
                Sched s = *it;
                sched_.erase(it);
                run(s, now);
            }
        }
    }

    // Earliest time tick() has something to do (for event-driven simulation)
    int64_t next_wakeup() const {
        int64_t t = INT64_MAX;
        if (!tx_busy_) {   // scheduled sends and reply timeouts wait for our unkeyed event
            for (const auto& s : sched_) t = std::min(t, s.at);
            if (wait_ != Wait::NONE && wait_ != Wait::IDLE) t = std::min(t, wait_until_);
        }
        if (state_ == State::CONNECTED || state_ == State::ACCEPTING || state_ == State::DISCONNECTING)
            t = std::min(t, last_rx_ms_ + p_.link_timeout_ms + 1);
        if (turn_req_at_ > 0) t = std::min(t, turn_req_at_);
        return t;
    }

    // ---- inspection ----

    State state() const { return state_; }
    bool is_iss() const { return iss_; }
    uint8_t bandwidth() const { return bw_; }
    int tx_step() const { return tx_step_; }
    int rx_step() const { return rx_step_; }
    size_t tx_pending() const { return tx_buf_.size(); }
    const std::string& peer() const { return peer_; }

private:
    enum class Action { CONNECT, CONNECT_ACK, REJECT, ISS_DECIDE, SEND_ACK, POLL, POLL_KEEPALIVE,
                        TURN_RESEND, DISC, DISC_ONCE, DISC_ACK, TURN_REQUEST };
    enum class Wait { NONE, CONNECT_REPLY, ACK, TURN_CONFIRM, DISC_ACK, BURST_END, IDLE };
    enum class Sent { OTHER, CONNECT, DATA, POLL, POLL_KEEPALIVE, TURN, DISC };

    struct Sched {
        Action action;
        int64_t at;
        uint16_t session;
        uint8_t reason;
    };

    using Range = std::pair<uint32_t, uint32_t>;   // [start, end)

    // ---------- connect / accept ----------

    void handle_connect(const Frame& f, float snr, int64_t now) {
        if (upper(f.dst) != mycall_ || mycall_.empty()) return;
        const std::string src = upper(f.src);
        if (state_ == State::CALLING && src == peer_) {
            // both ends called each other: the higher session ID wins
            if (f.session > session_) {
                log("simultaneous connect: accepting " + src + "'s call");
                cancel_all();
                state_ = State::DISCONNECTED;
            } else {
                return;
            }
        }
        if (state_ == State::DISCONNECTED) {
            if (!listen_) {
                schedule(Action::REJECT, now + p_.turnaround_ms, f.session, (uint8_t)RejectReason::NOT_LISTENING);
                return;
            }
            uint8_t common = f.flags & p_.bw_offer & supported_bw();
            if (!common || f.burst == 0) {
                schedule(Action::REJECT, now + p_.turnaround_ms, f.session, (uint8_t)RejectReason::BAD_PARAMS);
                return;
            }
            reset_session();
            session_ = f.session;
            peer_ = src;
            outgoing_ = false;
            iss_ = false;
            bw_ = (common & BW_WIDE) ? BW_WIDE : BW_NARROW;
            burst_ = std::min<int>(std::min<int>(f.burst, p_.burst_max), 8);
            connect_snr_ = clamp_snr(snr);
            state_ = State::ACCEPTING;
            last_rx_ms_ = now;
            last_activity_ = now;
            emit_simple(HostEventType::PENDING);
            log("call from " + peer_ + " accepted");
            schedule(Action::CONNECT_ACK, now + p_.turnaround_ms, session_);
            return;
        }
        if (f.session == session_ && !outgoing_) {
            // our CONNECT_ACK was lost; answer again
            if (!has(Action::CONNECT_ACK)) schedule(Action::CONNECT_ACK, now + p_.turnaround_ms, session_);
            return;
        }
        schedule(Action::REJECT, now + p_.turnaround_ms, f.session, (uint8_t)RejectReason::BUSY);
    }

    void handle_connect_ack(const Frame& f, int64_t now) {
        if (state_ != State::CALLING) return;
        uint8_t chosen = f.flags & BW_MASK;
        if (!(chosen & p_.bw_offer & supported_bw())) {
            close("rejected: bad parameters", now);
            return;
        }
        bw_ = chosen;
        burst_ = std::max(1, std::min<int>(f.burst, p_.burst_max));
        state_ = State::CONNECTED;
        iss_ = true;
        wait_ = Wait::NONE;
        HostEvent e{HostEventType::CONNECTED};
        e.local = mycall_;
        e.remote = peer_;
        e.outgoing = true;
        e.bw = bw_;
        host(e);
        log("connected to " + peer_);
        schedule(Action::ISS_DECIDE, now + p_.turnaround_ms);
    }

    void promote_accepted(int64_t /*now*/) {
        state_ = State::CONNECTED;
        cancel(Action::CONNECT_ACK);
        HostEvent e{HostEventType::CONNECTED};
        e.local = mycall_;
        e.remote = peer_;
        e.outgoing = false;
        e.bw = bw_;
        host(e);
        log("connected with " + peer_);
    }

    // ---------- ISS ----------

    void iss_decide(int64_t now) {
        if (state_ != State::CONNECTED || !iss_) return;
        uint32_t end = tx_end();
        if (disconnect_requested_ && tx_base_ == end) {
            start_disconnect(now);
            return;
        }
        auto lost = lost_ranges();
        bool have_new = tx_next_ < end && tx_next_ - tx_base_ < (uint32_t)p_.window_bytes;
        bool have_data = !lost.empty() || have_new;
        if (peer_has_data_ && (!have_data || bursts_since_peer_req_ >= p_.turn_fairness_bursts)) {
            send_turn(now);
            return;
        }
        if (have_data) {
            send_burst(lost, now);
            idle_poll_ms_ = p_.idle_poll_min_ms;
            return;
        }
        // nothing to send either way: keepalive poll, backing off
        wait_ = Wait::IDLE;
        schedule(Action::POLL_KEEPALIVE, now + idle_poll_ms_);
        idle_poll_ms_ = std::min(idle_poll_ms_ * 2, p_.idle_poll_max_ms);
    }

    void send_burst(const std::vector<Range>& lost, int64_t now) {
        const auto& ladder = p_.ladder(bw_);
        tx_step_ = std::max(0, std::min(tx_step_, (int)ladder.size() - 1));
        const ModeDesc& mode = ladder[tx_step_].mode;
        const uint32_t cap = (uint32_t)std::max<int>(1, mode.payload - (int)DATA_HEADER_SIZE);
        // the negotiated burst size, cut so one keying stays within burst_air_max_ms
        const int frames = std::max(1, std::min(burst_, mode.air_ms > 0 ? p_.burst_air_max_ms / mode.air_ms : burst_));
        std::vector<Range> segs;
        for (const auto& r : lost)
            for (uint32_t s = r.first; s < r.second && (int)segs.size() < frames; s += cap)
                segs.push_back({s, std::min(r.second, s + cap)});
        uint32_t limit = std::min<uint32_t>(tx_end(), tx_base_ + (uint32_t)p_.window_bytes);
        uint32_t s = tx_next_;
        while ((int)segs.size() < frames && s < limit) {
            uint32_t e = std::min(limit, s + cap);
            segs.push_back({s, e});
            s = e;
        }
        if (segs.empty()) return;
        tx_next_ = std::max(tx_next_, s);

        TxRequest req;
        req.mode = mode;
        int n = (int)segs.size();
        for (int i = 0; i < n; ++i) {
            Frame f;
            f.type = FrameType::DATA;
            f.session = session_;
            f.offset = segs[i].first;
            f.remaining = (uint8_t)(n - 1 - i);
            f.flags = (uint8_t)(((n - 1) << DATA_BURST_SHIFT) | (i == n - 1 ? DATA_LAST : 0));
            f.payload.assign(tx_buf_.begin() + (segs[i].first - tx_base_),
                             tx_buf_.begin() + (segs[i].second - tx_base_));
            req.frames.push_back(encode(f));
        }
        acked_before_burst_ = acked_bytes();
        burst_outstanding_ = true;
        if (peer_has_data_) bursts_since_peer_req_++;
        transmit(req, Sent::DATA, now);
    }

    void send_turn(int64_t now) {
        Frame f;
        f.type = FrameType::TURN;
        f.session = session_;
        f.next_expected = rx_cum_;
        iss_ = false;
        turn_pending_ = true;
        peer_has_data_ = false;
        bursts_since_peer_req_ = 0;
        log("handing the turn to " + peer_);
        transmit_one(f, p_.control_mode, TxWhen::IMMEDIATE, Sent::TURN, now);
    }

    void handle_ack(const Frame& f, int64_t now) {
        if (!iss_ || state_ != State::CONNECTED) return;
        // our stream
        if (f.cum > tx_base_ && f.cum <= tx_end()) advance_base(f.cum);
        for (const auto& r : f.sack) add_sacked(r.start, r.start + r.len);
        clip_sacked();
        // own step limit: a burst with nothing acknowledged drops two steps,
        // a fully acknowledged one lets the limit rise again
        const int top = (int)p_.ladder(bw_).size() - 1;
        if (burst_outstanding_) {
            if (acked_bytes() == acked_before_burst_) tx_cap_ = std::max(0, tx_step_ - 2);
            else if (tx_base_ == tx_next_) tx_cap_ = std::min(top, tx_cap_ + 1);
            burst_outstanding_ = false;
        }
        tx_step_ = std::max(0, std::min<int>({(int)f.step, tx_cap_, top}));
        peer_has_data_ = (f.flags & ACK_HAVE_DATA) != 0;
        if (!peer_has_data_) bursts_since_peer_req_ = 0;
        wait_ = Wait::NONE;
        report_buffer();
        schedule(Action::ISS_DECIDE, now + p_.turnaround_ms);
    }

    // ---------- IRS ----------

    void handle_data(const Frame& f, float snr, int air_ms, int64_t now) {
        if (iss_) return;                 // both think they hold the turn: ignore
        confirm_turn();
        if (!in_burst_) {
            in_burst_ = true;
            burst_heard_ = 0;
            burst_size_ = ((f.flags >> DATA_BURST_SHIFT) & 0x07) + 1;
            burst_snr_ = snr;
            burst_step_ = -1;
            burst_new_ = false;
        }
        // A full frame's size names its step; a short last segment matches none
        const auto& ladder = p_.ladder(bw_);
        for (int i = (int)ladder.size() - 1; i >= 0; --i)
            if (f.payload.size() + DATA_HEADER_SIZE == (size_t)ladder[i].mode.payload) {
                burst_step_ = std::max(burst_step_, i);
                break;
            }
        burst_heard_++;
        burst_size_ = std::max(burst_size_, ((f.flags >> DATA_BURST_SHIFT) & 0x07) + 1);
        burst_snr_ = std::min(burst_snr_, snr);
        if (store_rx(f.offset, f.payload)) burst_new_ = true;
        deliver_rx();
        if ((f.flags & DATA_LAST) || f.remaining == 0) {
            wait_ = Wait::NONE;
            schedule_ack(now + p_.turnaround_ms);
        } else {
            set_wait(Wait::BURST_END, now + (int64_t)f.remaining * air_ms + p_.burst_end_margin_ms);
        }
    }

    void handle_poll(const Frame& f, int64_t now) {
        if (iss_) {
            // the IRS asks for the turn
            if (f.flags & POLL_TURN_REQUEST) {
                peer_has_data_ = true;
                if (wait_ == Wait::IDLE) {
                    cancel(Action::POLL_KEEPALIVE);
                    wait_ = Wait::NONE;
                    schedule(Action::ISS_DECIDE, now + p_.turnaround_ms);
                }
            }
            return;
        }
        confirm_turn();
        if (wait_ == Wait::BURST_END) wait_ = Wait::NONE;
        schedule_ack(now + p_.turnaround_ms);
    }

    void handle_turn(const Frame& f, int64_t now) {
        if (iss_) return;                 // duplicate TURN after we already took it
        if (f.next_expected > tx_base_ && f.next_expected <= tx_end()) advance_base(f.next_expected);
        iss_ = true;
        turn_pending_ = false;
        in_burst_ = false;
        wait_ = Wait::NONE;
        cancel(Action::SEND_ACK);
        peer_has_data_ = false;
        bursts_since_peer_req_ = 0;
        tx_step_ = std::min(rx_step_, tx_cap_);
        turn_req_at_ = 0;
        log("took the turn from " + peer_);
        report_buffer();
        schedule(Action::ISS_DECIDE, now + p_.turnaround_ms);
    }

    void schedule_ack(int64_t at) {
        if (!has(Action::SEND_ACK)) schedule(Action::SEND_ACK, at);
    }

    void send_ack(int64_t now) {
        if (state_ != State::CONNECTED || iss_) return;
        if (in_burst_) finish_burst();
        if (disconnect_requested_ && tx_base_ == tx_end()) {
            start_disconnect(now);
            return;
        }
        Frame f;
        f.type = FrameType::ACK;
        f.session = session_;
        f.cum = rx_cum_;
        f.snr = last_burst_snr_;
        f.step = (uint8_t)rx_step_;
        f.flags = (tx_end() > tx_base_ ? ACK_HAVE_DATA : 0) | (last_burst_dup_ ? ACK_DUPLICATE : 0);
        f.rx_space_kib = 255;
        f.sack = sack_ranges(p_.control_mode.payload);
        transmit_one(f, p_.control_mode, TxWhen::IMMEDIATE, Sent::OTHER, now);
    }

    void finish_burst() {
        in_burst_ = false;
        last_burst_snr_ = clamp_snr(burst_snr_);
        last_burst_dup_ = !burst_new_;
        const auto& ladder = p_.ladder(bw_);
        const int top = (int)ladder.size() - 1;
        // Judge from the step the burst was sent at: our last suggestion can be stale,
        // since a new ISS starts from its own measurement of our signal
        if (burst_step_ >= 0) rx_step_ = std::min(burst_step_, top);
        if (burst_heard_ < burst_size_) {
            clean_bursts_ = 0;
            had_loss_ = true;
            if (burst_heard_ * 2 < burst_size_) rx_step_ -= 2;
            else if (burst_snr_ < ladder[std::min(rx_step_, top)].min_snr_db) rx_step_ -= 1;
        } else {
            ++clean_bursts_;
            // highest step this burst's SNR supports
            int target = rx_step_;
            while (target < top && burst_snr_ >= ladder[target + 1].min_snr_db + p_.up_snr_margin_db)
                ++target;
            // Until a burst loses frames, jump straight there; after that, climb one step
            // per up_after_clean_bursts clean bursts
            int need = had_loss_ ? p_.up_after_clean_bursts : 1;
            if (target > rx_step_ && clean_bursts_ >= need) {
                rx_step_ = had_loss_ ? rx_step_ + 1 : target;
                clean_bursts_ = 0;
            }
        }
        rx_step_ = std::max(0, std::min(rx_step_, top));
    }

    void confirm_turn() {
        if (turn_pending_) {
            turn_pending_ = false;
            if (wait_ == Wait::TURN_CONFIRM) wait_ = Wait::NONE;
        }
    }

    // An idle IRS with data asks for the turn instead of waiting for the next keepalive poll.
    // Only once the ISS has said it is idle (keepalive POLL): mid-cycle gaps are too short.
    void maybe_request_turn(int64_t now) {
        if (state_ != State::CONNECTED || iss_ || turn_pending_ || tx_busy_ || in_burst_ || !peer_idle_ ||
            tx_end() == tx_base_ || !sched_.empty()) {
            turn_req_at_ = 0;
            return;
        }
        if (turn_req_at_ == 0) {
            turn_req_at_ = last_activity_ + p_.ack_timeout_ms() + rand_between(0, p_.idle_poll_min_ms);
            return;
        }
        if (now >= turn_req_at_) {
            turn_req_at_ = 0;
            Frame f;
            f.type = FrameType::POLL;
            f.session = session_;
            f.flags = POLL_TURN_REQUEST;
            transmit_one(f, p_.control_mode, TxWhen::CONTEND, Sent::OTHER, now);
        }
    }

    // ---------- disconnect ----------

    void start_disconnect(int64_t now) {
        state_ = State::DISCONNECTING;
        disc_attempts_ = 0;
        schedule(Action::DISC, now);
    }

    void on_wait_expired(Wait w, int64_t now) {
        switch (w) {
        case Wait::CONNECT_REPLY:
            if (state_ != State::CALLING) return;
            if (connect_attempts_ >= p_.connect_attempts) close("connect failed: no answer", now);
            else schedule(Action::CONNECT, now);
            return;
        case Wait::ACK:
            if (state_ != State::CONNECTED || !iss_) return;
            if (++retries_ > p_.poll_retries) close("link failure: no acknowledgement", now);
            else schedule(Action::POLL, now);
            return;
        case Wait::TURN_CONFIRM:
            if (state_ != State::CONNECTED || iss_ || !turn_pending_) return;
            if (++retries_ > p_.poll_retries) close("link failure: turn not taken", now);
            else schedule(Action::TURN_RESEND, now);
            return;
        case Wait::DISC_ACK:
            if (state_ != State::DISCONNECTING) return;
            if (disc_attempts_ >= p_.disc_attempts) close("disconnected", now);
            else schedule(Action::DISC, now);
            return;
        case Wait::BURST_END:
            if (state_ == State::CONNECTED && !iss_ && in_burst_) schedule_ack(now);
            return;
        case Wait::NONE:
        case Wait::IDLE:
            return;
        }
    }

    void close(const std::string& reason, int64_t now) {
        State was = state_;
        if (session_) {
            last_session_ = session_;
            last_session_until_ = now + p_.last_session_hold_ms;
        }
        // keep only replies addressed to other sessions (DISC_ACK / REJECT / DISC_ONCE)
        sched_.erase(std::remove_if(sched_.begin(), sched_.end(),
                                    [](const Sched& s) {
                                        return s.action != Action::DISC_ACK && s.action != Action::REJECT &&
                                               s.action != Action::DISC_ONCE;
                                    }),
                     sched_.end());
        state_ = State::DISCONNECTED;
        wait_ = Wait::NONE;
        session_ = 0;
        tx_buf_.clear();
        report_buffer();
        if (was == State::ACCEPTING) {
            emit_simple(HostEventType::CANCEL_PENDING);
        } else if (was != State::DISCONNECTED) {
            HostEvent e{HostEventType::DISCONNECTED};
            e.remote = peer_;
            e.reason = reason;
            host(e);
        }
        log("session closed: " + reason);
    }

    // ---------- scheduled transmissions ----------

    void run(const Sched& s, int64_t now) {
        switch (s.action) {
        case Action::CONNECT: {
            if (state_ != State::CALLING) return;
            Frame f;
            f.type = FrameType::CONNECT;
            f.session = session_;
            f.flags = p_.bw_offer & supported_bw();
            f.burst = (uint8_t)std::max(1, std::min(p_.burst_max, 8));
            f.window_kib = (uint8_t)std::min(255, p_.window_bytes / 1024);
            f.src = mycall_;
            f.dst = peer_;
            connect_attempts_++;
            transmit_one(f, p_.connect_mode, TxWhen::CONTEND, Sent::CONNECT, now);
            return;
        }
        case Action::CONNECT_ACK: {
            if (state_ != State::ACCEPTING && state_ != State::CONNECTED) return;
            Frame f;
            f.type = FrameType::CONNECT_ACK;
            f.session = s.session;
            f.flags = bw_;
            f.burst = (uint8_t)burst_;
            f.window_kib = (uint8_t)std::min(255, p_.window_bytes / 1024);
            f.snr = connect_snr_;
            transmit_one(f, p_.connect_mode, TxWhen::IMMEDIATE, Sent::OTHER, now);
            return;
        }
        case Action::REJECT: {
            Frame f;
            f.type = FrameType::REJECT;
            f.session = s.session;
            f.reason = s.reason;
            transmit_one(f, p_.connect_mode, TxWhen::IMMEDIATE, Sent::OTHER, now);
            return;
        }
        case Action::ISS_DECIDE:
            iss_decide(now);
            return;
        case Action::SEND_ACK:
            send_ack(now);
            return;
        case Action::POLL:
        case Action::POLL_KEEPALIVE: {
            if (state_ != State::CONNECTED || !iss_) return;
            if (s.action == Action::POLL_KEEPALIVE) wait_ = Wait::NONE;
            Frame f;
            f.type = FrameType::POLL;
            f.session = session_;
            f.flags = s.action == Action::POLL_KEEPALIVE ? POLL_KEEPALIVE : 0;
            transmit_one(f, p_.control_mode, TxWhen::IMMEDIATE,
                         s.action == Action::POLL_KEEPALIVE ? Sent::POLL_KEEPALIVE : Sent::POLL, now);
            return;
        }
        case Action::TURN_RESEND: {
            if (state_ != State::CONNECTED || iss_ || !turn_pending_) return;
            Frame f;
            f.type = FrameType::TURN;
            f.session = session_;
            f.next_expected = rx_cum_;
            transmit_one(f, p_.control_mode, TxWhen::IMMEDIATE, Sent::TURN, now);
            return;
        }
        case Action::DISC: {
            if (state_ != State::DISCONNECTING) return;
            disc_attempts_++;
            Frame f;
            f.type = FrameType::DISC;
            f.session = session_;
            f.reason = (uint8_t)DiscReason::HOST;
            transmit_one(f, p_.connect_mode, TxWhen::IMMEDIATE, Sent::DISC, now);
            return;
        }
        case Action::DISC_ONCE:
        case Action::DISC_ACK: {
            Frame f;
            f.type = s.action == Action::DISC_ONCE ? FrameType::DISC : FrameType::DISC_ACK;
            f.session = s.session;
            f.reason = s.reason;
            transmit_one(f, p_.connect_mode, TxWhen::IMMEDIATE, Sent::OTHER, now);
            return;
        }
        case Action::TURN_REQUEST:
            return;
        }
    }

    void transmit_one(const Frame& f, const ModeDesc& mode, TxWhen when, Sent kind, int64_t now) {
        TxRequest req;
        req.mode = mode;
        req.when = when;
        req.frames.push_back(encode(f));
        transmit(req, kind, now);
    }

    void transmit(const TxRequest& req, Sent kind, int64_t now) {
        tx_busy_ = true;
        last_tx_ = kind;
        last_activity_ = now;
        if (on_transmit) on_transmit(req);
    }

    void schedule(Action a, int64_t at, uint16_t session = 0, uint8_t reason = 0) {
        sched_.push_back({a, at, session ? session : session_, reason});
    }
    void cancel(Action a) {
        sched_.erase(std::remove_if(sched_.begin(), sched_.end(), [a](const Sched& s) { return s.action == a; }),
                     sched_.end());
    }
    void cancel_all() { sched_.clear(); wait_ = Wait::NONE; }
    bool has(Action a) const {
        return std::any_of(sched_.begin(), sched_.end(), [a](const Sched& s) { return s.action == a; });
    }
    void set_wait(Wait w, int64_t until) {
        wait_ = w;
        wait_until_ = until;
    }

    // ---------- TX stream ----------

    uint32_t tx_end() const { return tx_base_ + (uint32_t)tx_buf_.size(); }

    void advance_base(uint32_t to) {
        tx_buf_.erase(tx_buf_.begin(), tx_buf_.begin() + (to - tx_base_));
        tx_base_ = to;
        tx_next_ = std::max(tx_next_, to);
        clip_sacked();
    }

    void add_sacked(uint32_t s, uint32_t e) {
        if (e <= s) return;
        sacked_.push_back({s, e});
        std::sort(sacked_.begin(), sacked_.end());
        std::vector<Range> merged;
        for (const auto& r : sacked_) {
            if (!merged.empty() && r.first <= merged.back().second)
                merged.back().second = std::max(merged.back().second, r.second);
            else
                merged.push_back(r);
        }
        sacked_.swap(merged);
    }

    void clip_sacked() {
        std::vector<Range> out;
        for (auto r : sacked_) {
            r.first = std::max(r.first, tx_base_);
            r.second = std::min(r.second, tx_next_);
            if (r.second > r.first) out.push_back(r);
        }
        sacked_.swap(out);
    }

    // Bytes sent but neither cumulatively nor selectively acknowledged
    std::vector<Range> lost_ranges() const {
        std::vector<Range> out;
        uint32_t s = tx_base_;
        for (const auto& r : sacked_) {
            if (r.first > s) out.push_back({s, r.first});
            s = std::max(s, r.second);
        }
        if (tx_next_ > s) out.push_back({s, tx_next_});
        return out;
    }

    uint64_t acked_bytes() const {
        uint64_t n = tx_base_;
        for (const auto& r : sacked_) n += r.second - r.first;
        return n;
    }

    // ---------- RX stream ----------

    // Returns true if any byte was new
    bool store_rx(uint32_t off, const std::vector<uint8_t>& data) {
        uint32_t s = off, e = off + (uint32_t)data.size();
        if (e <= rx_cum_) return false;
        s = std::max(s, rx_cum_);
        bool added = false;
        uint32_t pos = s;
        auto it = rx_ooo_.upper_bound(pos);
        if (it != rx_ooo_.begin()) {
            auto prev = std::prev(it);
            uint32_t pe = prev->first + (uint32_t)prev->second.size();
            if (pe > pos) pos = std::min(pe, e);
        }
        while (pos < e) {
            auto next = rx_ooo_.lower_bound(pos);
            uint32_t gap_end = (next == rx_ooo_.end()) ? e : std::min(e, next->first);
            if (gap_end > pos) {
                rx_ooo_[pos] = std::vector<uint8_t>(data.begin() + (pos - off), data.begin() + (gap_end - off));
                added = true;
            }
            if (next == rx_ooo_.end() || next->first >= e) break;
            pos = std::min(e, next->first + (uint32_t)next->second.size());
        }
        return added;
    }

    void deliver_rx() {
        std::vector<uint8_t> out;
        while (!rx_ooo_.empty() && rx_ooo_.begin()->first == rx_cum_) {
            auto& seg = rx_ooo_.begin()->second;
            out.insert(out.end(), seg.begin(), seg.end());
            rx_cum_ += (uint32_t)seg.size();
            rx_ooo_.erase(rx_ooo_.begin());
        }
        if (!out.empty() && on_rx_data) on_rx_data(out);
    }

    std::vector<SackRange> sack_ranges(int max_frame) const {
        size_t max_n = std::min<size_t>(MAX_SACK, (size_t)std::max(0, max_frame - 13) / 6);
        std::vector<SackRange> out;
        uint32_t cs = 0, ce = 0;
        bool open = false;
        auto flush = [&]() {
            while (open && ce > cs && out.size() < max_n) {
                uint32_t len = std::min<uint32_t>(ce - cs, 0xFFFF);
                out.push_back({cs, (uint16_t)len});
                cs += len;
                if (len < 0xFFFF) break;
            }
        };
        for (const auto& kv : rx_ooo_) {
            uint32_t s = kv.first, e = kv.first + (uint32_t)kv.second.size();
            if (open && s == ce) {
                ce = e;
            } else {
                flush();
                cs = s;
                ce = e;
                open = true;
            }
        }
        flush();
        return out;
    }

    // ---------- helpers ----------

    void reset_session() {
        cancel_all();
        tx_buf_.clear();
        tx_base_ = tx_next_ = 0;
        sacked_.clear();
        rx_cum_ = 0;
        rx_ooo_.clear();
        disconnect_requested_ = false;
        peer_has_data_ = false;
        bursts_since_peer_req_ = 0;
        turn_pending_ = false;
        in_burst_ = false;
        retries_ = 0;
        tx_step_ = rx_step_ = p_.start_step;
        tx_cap_ = (int)p_.ladder_narrow.size() - 1;
        if (!p_.ladder_wide.empty()) tx_cap_ = std::max(tx_cap_, (int)p_.ladder_wide.size() - 1);
        clean_bursts_ = 0;
        had_loss_ = false;
        burst_outstanding_ = false;
        idle_poll_ms_ = p_.idle_poll_min_ms;
        turn_req_at_ = 0;
        burst_ = std::max(1, std::min(p_.burst_max, 8));
        bw_ = BW_NARROW;
        last_buffer_ = SIZE_MAX;
    }

    uint8_t supported_bw() const { return BW_NARROW | (p_.ladder_wide.empty() ? 0 : BW_WIDE); }

    uint16_t random_session() {
        std::uniform_int_distribution<int> d(1, 0xFFFF);
        return (uint16_t)d(rng_);
    }
    int rand_between(int a, int b) {
        if (b <= a) return a;
        std::uniform_int_distribution<int> d(a, b);
        return d(rng_);
    }
    static int8_t clamp_snr(float snr) {
        return (int8_t)std::max(-128.0f, std::min(127.0f, snr));
    }
    static std::string upper(std::string s) {
        for (auto& c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    }
    static const char* reject_name(uint8_t r) {
        switch ((RejectReason)r) {
        case RejectReason::BUSY: return "busy";
        case RejectReason::NOT_LISTENING: return "not listening";
        case RejectReason::BAD_PARAMS: return "bad parameters";
        }
        return "unknown";
    }

    void report_buffer() {
        size_t n = tx_buf_.size();
        if (n == last_buffer_) return;
        last_buffer_ = n;
        HostEvent e{HostEventType::BUFFER};
        e.buffer = n;
        host(e);
    }
    void emit_simple(HostEventType t) {
        HostEvent e{t};
        e.remote = peer_;
        host(e);
    }
    void host(const HostEvent& e) {
        if (on_host_event) on_host_event(e);
    }
    void log(const std::string& m) {
        if (on_log) on_log(mycall_ + ": " + m);
    }

    Params p_;
    std::mt19937 rng_;

    std::string mycall_;
    bool listen_ = false;

    State state_ = State::DISCONNECTED;
    uint16_t session_ = 0;
    std::string peer_;
    bool outgoing_ = false;
    bool iss_ = false;
    uint8_t bw_ = BW_NARROW;
    int burst_ = 4;
    int8_t connect_snr_ = 0;

    std::vector<Sched> sched_;
    Wait wait_ = Wait::NONE;
    int64_t wait_until_ = 0;
    bool tx_busy_ = false;
    bool rx_busy_ = false;           // channel_busy(): a frame is being received
    Sent last_tx_ = Sent::OTHER;
    int retries_ = 0;
    int connect_attempts_ = 0;
    int disc_attempts_ = 0;
    int64_t last_rx_ms_ = 0;
    int64_t last_activity_ = 0;
    int idle_poll_ms_ = 3000;
    int64_t turn_req_at_ = 0;
    uint16_t last_session_ = 0;
    int64_t last_session_until_ = 0;
    bool disconnect_requested_ = false;
    uint16_t abort_disc_session_ = 0;  // aborted mid-transmission: send its DISC after unkeying

    // TX stream
    std::deque<uint8_t> tx_buf_;      // bytes from tx_base_ to the end of host data
    uint32_t tx_base_ = 0;            // everything before is acknowledged
    uint32_t tx_next_ = 0;            // everything before has been sent at least once
    std::vector<Range> sacked_;
    size_t last_buffer_ = SIZE_MAX;

    // RX stream
    uint32_t rx_cum_ = 0;
    std::map<uint32_t, std::vector<uint8_t>> rx_ooo_;

    // turn handling
    bool peer_has_data_ = false;
    int bursts_since_peer_req_ = 0;
    bool turn_pending_ = false;
    bool peer_idle_ = false;          // the ISS's last frame was a keepalive POLL

    // IRS burst tracking and AMC suggestion
    bool in_burst_ = false;
    int burst_heard_ = 0;
    int burst_size_ = 0;
    float burst_snr_ = 0;
    int burst_step_ = -1;             // step the ISS sent this burst at, from full frames' size
    bool burst_new_ = false;
    int8_t last_burst_snr_ = 0;
    bool last_burst_dup_ = false;
    int8_t last_snr_ = 0;
    int rx_step_ = 1;
    int clean_bursts_ = 0;
    bool had_loss_ = false;         // a burst this session lost frames: climb slowly

    // ISS AMC
    int tx_step_ = 1;
    int tx_cap_ = 6;
    bool burst_outstanding_ = false;
    uint64_t acked_before_burst_ = 0;
};

}  // namespace arq
