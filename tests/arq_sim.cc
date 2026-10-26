// ARQ engine simulator and tests (design proposal Section 8.11).
//
// Stations share one half-duplex channel with real frame airtimes, PTT lead/tail,
// collisions (a station hears nothing while it transmits, and overlapping
// transmissions destroy each other at every receiver), SNR-dependent frame loss,
// random loss, and scripted drops. Time is simulated; nothing sleeps.
//
// Build and run: make arq_test        Extra random seeds: ./tests/arq_sim 5000

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../arq/arq_session.hh"

using namespace arq;

// ---------------------------------------------------------------------------
// Simulated channel
// ---------------------------------------------------------------------------

struct Sim {
    struct Station {
        std::string call;
        std::unique_ptr<Link> link;
        std::vector<uint8_t> to_send;      // everything the host sent
        std::vector<uint8_t> received;     // everything delivered to the host
        std::vector<HostEvent> events;
        std::vector<std::pair<int64_t, int64_t>> tx_intervals;   // [key-down, key-up]
        std::set<std::string> modes_used;  // DATA modes this station transmitted
        int data_frames = 0;
        bool connected_seen = false;
        bool disconnected_seen = false;
        std::string disconnect_reason;
        int64_t connected_at = -1;
    };

    struct Ev {
        int64_t t;
        uint64_t seq;
        std::function<void()> fn;
        bool operator>(const Ev& o) const { return t != o.t ? t > o.t : seq > o.seq; }
    };

    Params params;
    std::mt19937 rng;
    std::vector<Station> st;
    std::priority_queue<Ev, std::vector<Ev>, std::greater<Ev>> q;
    uint64_t seq = 0;
    int64_t now = 0;
    bool verbose = false;

    // Channel model
    std::function<float(int from, int to, int64_t t)> snr = [](int, int, int64_t) { return 30.0f; };
    double random_loss = 0.0;
    // Return true to drop this frame (decoded ARQ frame, sender, receiver)
    std::function<bool(const Frame&, int from, int to)> drop = [](const Frame&, int, int) { return false; };
    bool channel_dead = false;
    int csma_quiet_ms = 1000;

    explicit Sim(uint32_t seed, Params p = Params()) : params(std::move(p)), rng(seed) {}

    int add(const std::string& call, bool listening) {
        int i = (int)st.size();
        st.emplace_back();
        Station& s = st.back();
        s.call = call;
        s.link = std::make_unique<Link>(params, rng());
        s.link->set_mycall(call);
        s.link->listen(listening);
        return i;
    }

    // Wire callbacks after all stations exist (vector growth would move them)
    void wire() {
        for (int i = 0; i < (int)st.size(); ++i) {
            Link& l = *st[i].link;
            l.on_transmit = [this, i](const TxRequest& r) { transmit(i, r); };
            l.on_rx_data = [this, i](const std::vector<uint8_t>& d) {
                st[i].received.insert(st[i].received.end(), d.begin(), d.end());
            };
            l.on_host_event = [this, i](const HostEvent& e) {
                Station& s = st[i];
                s.events.push_back(e);
                if (on_event) on_event(i, e);
                if (e.type == HostEventType::CONNECTED) {
                    s.connected_seen = true;
                    s.connected_at = now;
                    if (on_connected) on_connected(i);
                } else if (e.type == HostEventType::DISCONNECTED) {
                    s.disconnected_seen = true;
                    s.disconnect_reason = e.reason;
                }
            };
            if (verbose) l.on_log = [this](const std::string& m) { printf("  [%7.1fs] %s\n", now / 1000.0, m.c_str()); };
        }
    }

    std::function<void(int)> on_connected;
    std::function<void(int, const HostEvent&)> on_event;

    void at(int64_t t, std::function<void()> fn) { q.push({t, seq++, std::move(fn)}); }

    void send(int i, const std::vector<uint8_t>& d) {
        st[i].to_send.insert(st[i].to_send.end(), d.begin(), d.end());
        st[i].link->send(d, now);
    }

    bool busy_at(int except, int64_t t) const {
        for (int j = 0; j < (int)st.size(); ++j) {
            if (j == except) continue;
            for (const auto& iv : st[j].tx_intervals)
                if (iv.first <= t && t < iv.second) return true;
        }
        return false;
    }

    bool overlaps(int j, int64_t a, int64_t b) const {
        for (const auto& iv : st[j].tx_intervals)
            if (iv.first < b && a < iv.second) return true;
        return false;
    }

    // Frame loss probability from the SNR margin over the mode's requirement
    double loss_prob(const ModeDesc& m, float s) const {
        float need = 3.0f;   // control-frame modes
        if (m.name == params.connect_mode.name) need = 0.0f;
        for (const auto* l : {&params.ladder_narrow, &params.ladder_wide})
            for (const auto& step : *l)
                if (step.mode.name == m.name) need = (float)std::max(0, step.min_snr_db);
        double margin = s - need;
        double p_snr = 1.0 / (1.0 + std::exp(2.0 * (margin + 1.0)));
        return 1.0 - (1.0 - p_snr) * (1.0 - random_loss);
    }

    void transmit(int i, const TxRequest& r) {
        int64_t start = now;
        if (r.when == TxWhen::CONTEND) {
            // CSMA: wait for the channel to have been quiet for csma_quiet_ms, plus jitter
            std::uniform_int_distribution<int> jitter(0, 400);
            for (bool moved = true; moved;) {
                moved = false;
                for (int j = 0; j < (int)st.size(); ++j) {
                    if (j == i) continue;
                    for (const auto& iv : st[j].tx_intervals)
                        if (iv.first <= start && start < iv.second + csma_quiet_ms) {
                            start = iv.second + csma_quiet_ms;
                            moved = true;
                        }
                }
            }
            start += jitter(rng);
        }
        int64_t t = start + params.key_lead_ms;
        std::vector<std::pair<int64_t, int64_t>> frame_iv;
        for (size_t k = 0; k < r.frames.size(); ++k) {
            frame_iv.push_back({t, t + r.mode.air_ms});
            t += r.mode.air_ms;
        }
        int64_t end = t + params.key_tail_ms;
        st[i].tx_intervals.push_back({start, end});
        if (verbose) {
            Frame f0;
            decode(r.frames[0].data(), r.frames[0].size(), f0);
            static const char* names[] = {"?", "CONNECT", "CONNECT_ACK", "REJECT", "DATA", "ACK",
                                          "POLL", "TURN", "DISC", "DISC_ACK"};
            printf("  [%7.1fs] %s TX %s x%zu %s %.1f-%.1fs", now / 1000.0, st[i].call.c_str(),
                   names[(int)f0.type], r.frames.size(), r.mode.name.c_str(), start / 1000.0, end / 1000.0);
            if (f0.type == FrameType::ACK) printf(" cum=%u sack=%zu step=%d flags=%d", f0.cum, f0.sack.size(), f0.step, f0.flags);
            if (f0.type == FrameType::DATA) printf(" off=%u", f0.offset);
            if (f0.type == FrameType::POLL) printf(" flags=%d", f0.flags);
            printf("\n");
        }
        at(start, [this, i]() { st[i].link->tx_event(TxEventType::KEYED, now); });
        for (size_t k = 0; k < r.frames.size(); ++k) {
            Frame f;
            decode(r.frames[k].data(), r.frames[k].size(), f);
            if (f.type == FrameType::DATA) {
                st[i].data_frames++;
                st[i].modes_used.insert(r.mode.name);
            }
            auto bytes = r.frames[k];
            auto iv = frame_iv[k];
            ModeDesc mode = r.mode;
            at(iv.second, [this, i, bytes, iv, mode, f]() {
                for (int j = 0; j < (int)st.size(); ++j) {
                    if (j == i) continue;
                    auto lost = [&](const char* why) {
                        if (verbose) printf("  [%7.1fs]   -> %s lost (%s)\n", now / 1000.0, st[j].call.c_str(), why);
                    };
                    if (channel_dead) { lost("channel dead"); continue; }
                    if (overlaps(j, iv.first, iv.second)) { lost("receiver transmitting"); continue; }
                    bool other_tx = false;   // someone else on air at the same time
                    for (int k2 = 0; k2 < (int)st.size(); ++k2)
                        if (k2 != i && k2 != j && overlaps(k2, iv.first, iv.second)) other_tx = true;
                    if (other_tx) { lost("collision"); continue; }
                    float s = snr(i, j, now);
                    std::uniform_real_distribution<double> u(0, 1);
                    if (u(rng) < loss_prob(mode, s)) { lost("noise"); continue; }
                    if (drop(f, i, j)) { lost("scripted"); continue; }
                    st[j].link->frame_received(bytes, s, mode.air_ms, now);
                }
            });
        }
        at(end, [this, i]() { st[i].link->tx_event(TxEventType::UNKEYED, now); });
    }

    // Run until `done` or the time limit
    void run(int64_t limit_ms, std::function<bool()> done) {
        while (now < limit_ms) {
            while (!q.empty() && q.top().t <= now) {
                Ev e = q.top();
                q.pop();
                e.fn();
            }
            for (int pass = 0; pass < 8; ++pass)
                for (auto& s : st) s.link->tick(now);
            if (done && done()) break;
            int64_t next = q.empty() ? INT64_MAX : q.top().t;
            for (auto& s : st) next = std::min(next, s.link->next_wakeup());
            if (next == INT64_MAX) break;
            now = next > now ? next : now + 1;   // always make progress
        }
    }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::vector<uint8_t> pattern(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    std::mt19937 r(seed);
    for (auto& b : v) b = (uint8_t)r();
    return v;
}

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("    FAIL %s\n", what.c_str());
    }
}

static bool is_prefix(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    return a.size() <= b.size() && std::equal(a.begin(), a.end(), b.begin());
}

struct TransferResult {
    bool complete = false;      // both streams delivered and both ends disconnected cleanly
    bool prefix_ok = true;      // each received stream is a prefix of what was sent
    bool terminated = false;    // both ends DISCONNECTED
    double seconds = 0;
    std::string reason_a, reason_b;
};

// A calls B; each sends its data once connected; A disconnects when both directions are done.
static TransferResult transfer(Sim& sim, size_t a_bytes, size_t b_bytes, int64_t limit_ms = 30 * 60 * 1000,
                               bool print = false) {
    int A = 0, B = 1;
    auto data_a = pattern(a_bytes, 1);
    auto data_b = pattern(b_bytes, 2);
    sim.on_connected = [&](int i) {
        if (i == A && a_bytes) sim.send(A, data_a);
        if (i == B && b_bytes) sim.send(B, data_b);
    };
    sim.wire();
    sim.st[A].link->connect(sim.st[B].call, 0);
    bool disc_sent = false;
    sim.run(limit_ms, [&]() {
        auto& a = sim.st[A];
        auto& b = sim.st[B];
        if (!disc_sent && b.received.size() == a_bytes && a.received.size() == b_bytes && a.connected_seen &&
            a.link->state() == State::CONNECTED) {
            a.link->disconnect(sim.now);
            disc_sent = true;
        }
        return a.link->state() == State::DISCONNECTED && b.link->state() == State::DISCONNECTED &&
               (disc_sent || a.disconnected_seen);
    });
    TransferResult r;
    auto& a = sim.st[A];
    auto& b = sim.st[B];
    r.prefix_ok = is_prefix(b.received, data_a) && is_prefix(a.received, data_b);
    r.terminated = a.link->state() == State::DISCONNECTED && b.link->state() == State::DISCONNECTED;
    r.complete = r.terminated && b.received == data_a && a.received == data_b && disc_sent;
    r.seconds = sim.now / 1000.0;
    r.reason_a = a.disconnect_reason;
    r.reason_b = b.disconnect_reason;
    if (print) {
        double t = (a.connected_at >= 0) ? (sim.now - a.connected_at) / 1000.0 : r.seconds;
        printf("    %zu B A->B, %zu B B->A in %.1f s (%.0f bps payload), A frames %d, B frames %d, A modes:",
               a_bytes, b_bytes, t, t > 0 ? (a_bytes + b_bytes) * 8 / t : 0.0, a.data_frames, b.data_frames);
        for (auto& m : a.modes_used) printf(" [%s]", m.c_str());
        printf("\n");
    }
    return r;
}

static void expect_complete(const TransferResult& r, const char* name) {
    check(r.prefix_ok, std::string(name) + ": received streams are prefixes of sent streams");
    check(r.complete, std::string(name) + ": transfer complete and both ends disconnected (A: " + r.reason_a +
                          ", B: " + r.reason_b + ")");
}

// Drop the first `n` frames of `type` (optionally only DATA frames at a burst position)
static std::function<bool(const Frame&, int, int)> drop_first(FrameType type, int n,
                                                              std::function<bool(const Frame&)> sel = nullptr) {
    auto count = std::make_shared<int>(0);
    return [=](const Frame& f, int, int) {
        if (f.type != type || (sel && !sel(f))) return false;
        if (*count < n) {
            ++*count;
            return true;
        }
        return false;
    };
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#define TEST(name) static void name()
#define RUN(name) do { printf("%s\n", #name); name(); } while (0)

TEST(frames_roundtrip) {
    Frame c;
    c.type = FrameType::CONNECT;
    c.session = 0xBEEF;
    c.flags = BW_NARROW | BW_WIDE;
    c.burst = 4;
    c.window_kib = 64;
    c.src = "N0CALL-10";
    c.dst = "VE3XYZ-1";
    auto b = encode(c);
    Frame d;
    check(decode(b.data(), b.size(), d) && d.src == c.src && d.dst == c.dst && d.session == 0xBEEF &&
              d.flags == c.flags,
          "CONNECT round trip");
    check(b.size() <= 30, "CONNECT fits a 30-byte micro frame (" + std::to_string(b.size()) + " B)");

    Frame a;
    a.type = FrameType::ACK;
    a.session = 7;
    a.cum = 123456;
    a.snr = -5;
    a.step = 3;
    a.sack = {{200000, 1000}, {300000, 65535}, {400000, 1}};
    b = encode(a);
    check(decode(b.data(), b.size(), d) && d.cum == 123456 && d.snr == -5 && d.sack.size() == 3 &&
              d.sack[1].len == 65535,
          "ACK round trip");
    check(b.size() == 31, "ACK with 3 SACK ranges is 31 bytes");

    std::vector<uint8_t> ax25 = {0x9C, 0x60, 0x86, 0x82, 0x98, 0x98, 0x60};
    check(!decode(ax25.data(), ax25.size(), d), "AX.25 frame is not mistaken for ARQ");
    std::vector<uint8_t> v2 = {MAGIC, 0x24, 0, 1, 0};
    check(!decode(v2.data(), v2.size(), d), "unknown protocol version rejected");
    std::vector<uint8_t> trunc = {MAGIC, 0x15, 0, 1, 0, 0};
    check(!decode(trunc.data(), trunc.size(), d), "truncated ACK rejected");
}

TEST(clean_one_way) {
    Sim sim(1);
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    auto r = transfer(sim, 50000, 0, 30 * 60 * 1000, true);
    expect_complete(r, "clean 50 kB");
    check(sim.st[0].link->tx_step() >= 5 || sim.st[0].modes_used.count("QAM256 5/6 normal"),
          "link adapted up to the top steps on a 30 dB channel");
}

TEST(clean_bidirectional) {
    Sim sim(2);
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    auto r = transfer(sim, 30000, 20000, 30 * 60 * 1000, true);
    expect_complete(r, "bidirectional 30 kB / 20 kB");
}

TEST(drop_first_frame_of_burst) {
    Sim sim(3);
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    sim.drop = drop_first(FrameType::DATA, 1, [](const Frame& f) { return f.offset == 0; });
    expect_complete(transfer(sim, 8000, 0), "first DATA frame lost");
}

TEST(drop_last_frame_of_burst) {
    Sim sim(4);
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    sim.drop = drop_first(FrameType::DATA, 2, [](const Frame& f) { return (f.flags & DATA_LAST) != 0; });
    expect_complete(transfer(sim, 8000, 0), "last DATA frame of the first two bursts lost");
}

TEST(drop_middle_frames) {
    Sim sim(5);
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    sim.drop = drop_first(FrameType::DATA, 3, [](const Frame& f) { return f.remaining == 1 || f.remaining == 2; });
    expect_complete(transfer(sim, 8000, 0), "middle DATA frames lost");
}

TEST(drop_acks) {
    Sim sim(6);
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    sim.drop = drop_first(FrameType::ACK, 3);
    expect_complete(transfer(sim, 8000, 0), "first three ACKs lost");
}

TEST(drop_connect_ack) {
    Sim sim(7);
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    sim.drop = drop_first(FrameType::CONNECT_ACK, 2);
    expect_complete(transfer(sim, 3000, 0), "first two CONNECT_ACKs lost");
}

TEST(drop_turn) {
    Sim sim(8);
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    sim.drop = drop_first(FrameType::TURN, 1);
    expect_complete(transfer(sim, 3000, 3000), "first TURN lost");
}

TEST(drop_disc_ack) {
    Sim sim(9);
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    sim.drop = drop_first(FrameType::DISC_ACK, 1);
    expect_complete(transfer(sim, 2000, 0), "first DISC_ACK lost");
}

TEST(mode_change_mid_hole) {
    // Clean at first so the link climbs, then the SNR collapses: holes from the fast
    // mode must be resent re-chunked in slower modes.
    Sim sim(10);
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    sim.snr = [](int, int, int64_t t) { return t < 250000 ? 30.0f : 10.0f; };
    auto r = transfer(sim, 400000, 0, 2 * 60 * 60 * 1000, true);
    expect_complete(r, "SNR drop from 30 to 10 dB mid-transfer");
    check(sim.st[0].modes_used.size() >= 3, "several ladder steps used");
    check(sim.st[0].link->tx_step() <= 3, "ended on a step suited to 10 dB");
}

TEST(random_loss_levels) {
    for (double loss : {0.01, 0.05, 0.10, 0.20, 0.30}) {
        int complete = 0, runs = 20;
        for (int s = 0; s < runs; ++s) {
            Sim sim(1000 + s);
            sim.random_loss = loss;
            sim.add("ALPHA", false);
            sim.add("BRAVO", true);
            auto r = transfer(sim, 10000, 4000, 2 * 60 * 60 * 1000);
            check(r.prefix_ok, "random loss: prefix invariant");
            check(r.terminated, "random loss: session terminated");
            if (r.complete) complete++;
        }
        printf("    random loss %2.0f%%: %d/%d complete\n", loss * 100, complete, runs);
        if (loss <= 0.10) check(complete == runs, "every transfer completes at <=10% loss");
    }
}

TEST(simultaneous_connect) {
    Sim sim(11);
    sim.add("ALPHA", true);
    sim.add("BRAVO", true);
    sim.wire();
    sim.st[0].link->connect("BRAVO", 0);
    sim.st[1].link->connect("ALPHA", 0);
    sim.run(5 * 60 * 1000, [&]() {
        return sim.st[0].link->state() == State::CONNECTED && sim.st[1].link->state() == State::CONNECTED;
    });
    check(sim.st[0].link->state() == State::CONNECTED && sim.st[1].link->state() == State::CONNECTED,
          "both calls resolve into one session");
    check(sim.st[0].link->is_iss() != sim.st[1].link->is_iss(), "exactly one side holds the turn");
}

TEST(disconnect_from_irs) {
    Sim sim(12);
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    auto data_b = pattern(6000, 2);
    sim.on_connected = [&](int i) {
        if (i == 1) {
            sim.send(1, data_b);
            sim.st[1].link->disconnect(sim.now);   // graceful: flush first
        }
    };
    sim.wire();
    sim.st[0].link->connect("BRAVO", 0);
    sim.run(30 * 60 * 1000, [&]() {
        return sim.st[0].link->state() == State::DISCONNECTED && sim.st[1].link->state() == State::DISCONNECTED &&
               sim.st[0].disconnected_seen;
    });
    check(sim.st[0].received == data_b, "IRS's buffered data delivered before its disconnect");
    check(sim.st[0].disconnected_seen && sim.st[1].disconnected_seen, "both ends report DISCONNECTED");
    check(sim.st[0].disconnect_reason == "remote disconnected", "caller sees a remote disconnect (" +
                                                                    sim.st[0].disconnect_reason + ")");
}

TEST(abort_mid_transfer) {
    Sim sim(13);
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    auto data = pattern(40000, 1);
    sim.on_connected = [&](int i) {
        if (i == 0) sim.send(0, data);
    };
    sim.wire();
    sim.st[0].link->connect("BRAVO", 0);
    sim.at(40000, [&]() { sim.st[0].link->abort(sim.now); });
    sim.run(10 * 60 * 1000, [&]() {
        return sim.now > 40000 && sim.st[0].link->state() == State::DISCONNECTED &&
               sim.st[1].link->state() == State::DISCONNECTED;
    });
    check(sim.st[0].link->state() == State::DISCONNECTED, "aborting side disconnects at once");
    check(sim.st[1].link->state() == State::DISCONNECTED && sim.now < 40000 + 30000,
          "peer disconnects within 30 s (" + std::to_string(sim.now / 1000) + " s)");
    check(is_prefix(sim.st[1].received, data), "partial data is a clean prefix");
    size_t last_buffer = SIZE_MAX, max_buffer = 0;
    for (const auto& e : sim.st[0].events)
        if (e.type == HostEventType::BUFFER) {
            last_buffer = e.buffer;
            max_buffer = std::max(max_buffer, e.buffer);
        }
    check(last_buffer == 0, "BUFFER reads 0 after the abort (" + std::to_string(last_buffer) + ")");
    check(max_buffer <= data.size(), "BUFFER never above what the host sent");
}

TEST(link_failure) {
    Sim sim(14);
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    auto data = pattern(200000, 1);
    sim.on_connected = [&](int i) {
        if (i == 0) sim.send(0, data);
    };
    sim.wire();
    sim.st[0].link->connect("BRAVO", 0);
    sim.at(30000, [&]() { sim.channel_dead = true; });
    sim.run(10 * 60 * 1000, [&]() {
        return sim.st[0].link->state() == State::DISCONNECTED && sim.st[1].link->state() == State::DISCONNECTED;
    });
    check(sim.st[0].disconnected_seen && sim.st[1].disconnected_seen, "both ends give up");
    check(sim.now < 30000 + Params().link_timeout_ms + 10000, "within the link timeout");
    check(sim.st[0].disconnect_reason.find("link failure") == 0, "ISS reports link failure (" +
                                                                     sim.st[0].disconnect_reason + ")");
    check(is_prefix(sim.st[1].received, data), "partial data is a clean prefix");
}

TEST(connect_not_listening) {
    Sim sim(15);
    sim.add("ALPHA", false);
    sim.add("BRAVO", false);
    sim.wire();
    sim.st[0].link->connect("BRAVO", 0);
    sim.run(5 * 60 * 1000, [&]() { return sim.st[0].disconnected_seen; });
    check(sim.st[0].disconnect_reason == "rejected: not listening", "rejected: not listening (" +
                                                                        sim.st[0].disconnect_reason + ")");
}

TEST(connect_no_answer) {
    Sim sim(16);
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    sim.wire();
    sim.st[0].link->connect("CHARLIE", 0);
    sim.run(5 * 60 * 1000, [&]() { return sim.st[0].disconnected_seen; });
    check(sim.st[0].disconnect_reason == "connect failed: no answer", "connect fails after retries");
    check(sim.now >= 8 * 4000 && sim.now <= 8 * 6000 + 20000,
          "after about 8 attempts (" + std::to_string(sim.now / 1000) + " s)");
}

TEST(busy_third_station) {
    // A-B session idles (keepalive gaps let CSMA in); CHARLIE calls BRAVO and is turned away,
    // then A sends data and the session still works.
    Sim sim(17);
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    sim.add("CHARLIE", false);
    auto data = pattern(20000, 1);
    sim.wire();
    sim.st[0].link->connect("BRAVO", 0);
    sim.at(20000, [&]() { sim.st[2].link->connect("BRAVO", sim.now); });
    sim.run(5 * 60 * 1000, [&]() { return sim.st[2].disconnected_seen; });
    check(sim.st[2].disconnect_reason == "rejected: busy", "third station gets REJECT busy (" +
                                                               sim.st[2].disconnect_reason + ")");
    sim.send(0, data);
    sim.run(sim.now + 30 * 60 * 1000, [&]() { return sim.st[1].received.size() == data.size(); });
    check(sim.st[1].received == data, "the existing session still carries data afterwards");
}

TEST(idle_then_data) {
    // Connected and idle long enough for keepalive back-off, then the IRS has data
    Sim sim(18);
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    auto data = pattern(3000, 2);
    sim.wire();
    sim.st[0].link->connect("BRAVO", 0);
    sim.at(120000, [&]() { sim.send(1, data); });
    sim.run(10 * 60 * 1000, [&]() { return sim.st[0].received.size() == data.size(); });
    check(sim.st[0].received == data, "IRS data delivered after an idle period");
    check(sim.now < 120000 + 60000, "within a minute of the host sending (" + std::to_string(sim.now / 1000 - 120) +
                                        " s)");
    check(sim.st[0].link->state() == State::CONNECTED, "link stayed up through the idle period");
}

static Params wide_params();

static void random_seeds_run(bool wide, int seeds, int seed_base) {
    int complete = 0, failed_link = 0, bad = 0;
    for (int s = 0; s < seeds; ++s) {
        std::mt19937 r(seed_base + s);
        Sim sim(r(), wide ? wide_params() : Params());
        sim.random_loss = std::uniform_real_distribution<double>(0.0, 0.3)(r);
        float base = std::uniform_real_distribution<float>(8.0f, 30.0f)(r);
        sim.snr = [base](int, int, int64_t t) { return base + 3.0f * std::sin(t / 20000.0); };
        sim.add("ALPHA", false);
        sim.add("BRAVO", true);
        size_t a = r() % 20000, b = r() % 8000;
        auto res = transfer(sim, a, b, 3 * 60 * 60 * 1000);
        if (!res.prefix_ok || !res.terminated) {
            bad++;
            if (bad <= 5)
                printf("    seed %d: prefix_ok=%d terminated=%d (loss %.2f, snr %.1f, %zu/%zu B)\n", seed_base + s,
                       res.prefix_ok, res.terminated, sim.random_loss, base, a, b);
        } else if (res.complete) {
            complete++;
        } else {
            failed_link++;
        }
    }
    printf("    %s%d seeds: %d complete, %d ended by link failure, %d invariant violations\n", wide ? "wide: " : "",
           seeds, complete, failed_link, bad);
    check(bad == 0, "no invariant violations across random seeds");
    // Channels here go down to ~5 dB SNR with up to 30% extra loss, so some link failures are expected
    check(complete >= seeds * 90 / 100, "at least 90% of random transfers complete");
}

TEST(random_seeds) {
    int seeds = 1000;
    if (const char* e = getenv("ARQ_SEEDS")) seeds = atoi(e);
    random_seeds_run(false, seeds, 50000);
    random_seeds_run(true, std::max(1, seeds / 2), 70000);
}

static Params wide_params() {
    Params p;
    p.bw_offer = BW_NARROW | BW_WIDE;
    return p;
}

static uint8_t connected_bw(const Sim::Station& s) {
    for (const auto& e : s.events)
        if (e.type == HostEventType::CONNECTED) return e.bw;
    return 0;
}

static bool all_wide(const std::set<std::string>& modes) {
    for (const auto& m : modes)
        if (m.size() < 5 || m.compare(m.size() - 5, 5, " wide") != 0) return false;
    return !modes.empty();
}

TEST(wide_negotiation) {
    {
        Sim sim(30, wide_params());
        sim.add("ALPHA", false);
        sim.add("BRAVO", true);
        expect_complete(transfer(sim, 20000, 5000), "both offer wide");
        check(connected_bw(sim.st[0]) == BW_WIDE && connected_bw(sim.st[1]) == BW_WIDE,
              "both ends connected wide");
        check(all_wide(sim.st[0].modes_used) && all_wide(sim.st[1].modes_used), "every DATA frame was wide");
    }
    for (int narrow_end = 0; narrow_end < 2; ++narrow_end) {
        Sim sim(31 + narrow_end, wide_params());
        sim.add("ALPHA", false);
        sim.add("BRAVO", true);
        sim.st[narrow_end].link->set_bw_offer(BW_NARROW);
        expect_complete(transfer(sim, 8000, 0), narrow_end ? "caller wide, called narrow" : "caller narrow, called wide");
        check(connected_bw(sim.st[0]) == BW_NARROW && connected_bw(sim.st[1]) == BW_NARROW,
              "fell back to narrow");
        for (const auto& m : sim.st[0].modes_used)
            check(m.find("wide") == std::string::npos, "no wide DATA frames in a narrow session (" + m + ")");
    }
    {
        Params p = wide_params();
        p.ladder_wide.clear();   // a station without the wide decoder
        Sim sim(33, p);
        sim.add("ALPHA", false);
        sim.add("BRAVO", true);
        expect_complete(transfer(sim, 4000, 0), "wide offered, not supported");
        check(connected_bw(sim.st[0]) == BW_NARROW, "unsupported wide is not negotiated");
    }
}

TEST(wide_clean_climbs) {
    Sim sim(34, wide_params());
    sim.snr = [](int, int, int64_t) { return 33.0f; };   // the top step needs 30 + 2
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    auto r = transfer(sim, 200000, 0, 30 * 60 * 1000, true);
    expect_complete(r, "wide clean 200 kB");
    check(sim.st[0].modes_used.count("QAM1024 5/6 normal wide") == 1, "wide link reached its top step at 33 dB");
}

TEST(wide_step_by_airtime) {
    // At 20.5 dB the link should settle at QAM64 5/6 (18 + 2 dB; the next step needs 20 + 2).
    // QAM256 5/6 and QAM1024 5/6 have the same payload: read by size alone, the receiver
    // would take a QAM64 5/6 burst for QAM1024 5/6 and suggest from there
    Sim sim(35, wide_params());
    sim.snr = [](int, int, int64_t) { return 20.5f; };
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    auto r = transfer(sim, 150000, 0, 30 * 60 * 1000, true);
    expect_complete(r, "wide 150 kB at 20.5 dB");
    check(sim.st[0].modes_used.count("QAM64 5/6 long wide") == 1, "used QAM64 5/6 at 20.5 dB");
    bool above = false;
    for (const auto& m : sim.st[0].modes_used) above |= m.rfind("QAM256", 0) == 0 || m.rfind("QAM1024", 0) == 0;
    check(!above, "never sent above the channel's step");
}

// A sends `bytes` to B the way Pat writes: 127-byte B2F blocks, holding off while the
// last BUFFER report is at or above 7 blocks (889 B). Returns seconds from connect to
// B having everything, or -1.
static double paced_transfer(Sim& sim, size_t bytes) {
    const size_t block = 127, limit = 7 * block;
    auto data = pattern(bytes, 3);
    size_t written = 0, count = 0;
    bool pumping = false;
    std::function<void()> pump = [&]() {
        pumping = false;
        while (written < data.size() && count < limit) {
            size_t n = std::min(block, data.size() - written);
            count += n;
            sim.send(0, std::vector<uint8_t>(data.begin() + written, data.begin() + written + n));
            written += n;
        }
    };
    auto wake = [&]() {
        if (!pumping) { pumping = true; sim.at(sim.now, pump); }
    };
    sim.on_connected = [&](int i) { if (i == 0) wake(); };
    sim.on_event = [&](int i, const HostEvent& e) {
        if (i != 0 || e.type != HostEventType::BUFFER) return;
        count = e.buffer;
        wake();
    };
    sim.wire();
    sim.st[0].link->connect(sim.st[1].call, 0);
    sim.run(30 * 60 * 1000, [&]() { return sim.st[1].received.size() >= data.size(); });
    if (sim.st[1].received != data || sim.st[0].connected_at < 0) return -1;
    return (sim.now - sim.st[0].connected_at) / 1000.0;
}

TEST(paced_host_fills_frames) {
    // A host pacing itself by BUFFER must still get full frames: BUFFER leaves out
    // what the next burst will carry
    for (int wide = 0; wide < 2; ++wide) {
        const char* bw = wide ? "wide" : "narrow";
        Sim bulk_sim(40 + wide, wide ? wide_params() : Params());
        bulk_sim.add("ALPHA", false);
        bulk_sim.add("BRAVO", true);
        auto r = transfer(bulk_sim, 51200, 0);
        expect_complete(r, (std::string(bw) + " bulk 50 kB").c_str());
        double bulk = (bulk_sim.now - bulk_sim.st[0].connected_at) / 1000.0;

        Sim sim(40 + wide, wide ? wide_params() : Params());
        sim.add("ALPHA", false);
        sim.add("BRAVO", true);
        double paced = paced_transfer(sim, 51200);
        printf("    %s 50 kB: paced host %.1f s, bulk %.1f s, %d DATA frames\n", bw, paced, bulk,
               sim.st[0].data_frames);
        check(paced > 0, std::string(bw) + ": paced host's 50 kB delivered intact");
        check(paced > 0 && paced < bulk * 1.25, std::string(bw) + ": paced host within 25% of bulk time");
    }
}

// ./arq_sim trace <seed> <loss> <a_bytes> <b_bytes>: one verbose transfer
static int trace(int argc, char** argv) {
    uint32_t seed = argc > 2 ? atoi(argv[2]) : 1;
    Sim sim(seed);
    sim.verbose = true;
    sim.random_loss = argc > 3 ? atof(argv[3]) : 0.0;
    sim.add("ALPHA", false);
    sim.add("BRAVO", true);
    auto r = transfer(sim, argc > 4 ? atoi(argv[4]) : 10000, argc > 5 ? atoi(argv[5]) : 4000,
                      2 * 60 * 60 * 1000, true);
    printf("complete=%d prefix_ok=%d terminated=%d A: %s B: %s\n", r.complete, r.prefix_ok, r.terminated,
           r.reason_a.c_str(), r.reason_b.c_str());
    return 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc > 1 && std::string(argv[1]) == "trace") return trace(argc, argv);
    if (argc > 1) setenv("ARQ_SEEDS", argv[1], 1);
    RUN(frames_roundtrip);
    RUN(clean_one_way);
    RUN(clean_bidirectional);
    RUN(drop_first_frame_of_burst);
    RUN(drop_last_frame_of_burst);
    RUN(drop_middle_frames);
    RUN(drop_acks);
    RUN(drop_connect_ack);
    RUN(drop_turn);
    RUN(drop_disc_ack);
    RUN(mode_change_mid_hole);
    RUN(random_loss_levels);
    RUN(simultaneous_connect);
    RUN(disconnect_from_irs);
    RUN(abort_mid_transfer);
    RUN(link_failure);
    RUN(connect_not_listening);
    RUN(connect_no_answer);
    RUN(busy_third_station);
    RUN(idle_then_data);
    RUN(wide_negotiation);
    RUN(wide_clean_climbs);
    RUN(wide_step_by_airtime);
    RUN(paced_host_fills_frames);
    RUN(random_seeds);
    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
