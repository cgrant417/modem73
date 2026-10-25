// VARA host protocol test: two VaraHost instances driven the way a VARA host (Pat,
// LinBPQ) drives VARA, joined by a simulated half-duplex radio channel with real frame
// airtimes, PTT lead/tail and optional random frame loss. Time is simulated.
//
// Build and run: make vara_test

#include <cstdio>
#include <functional>
#include <memory>
#include <queue>
#include <random>
#include <string>
#include <vector>

#include "../vara_host.hh"

struct Sim {
    struct Ev {
        int64_t t;
        uint64_t seq;
        std::function<void()> fn;
        bool operator>(const Ev& o) const { return t != o.t ? t > o.t : seq > o.seq; }
    };
    struct Station {
        std::unique_ptr<VaraHost> vh;
        std::vector<std::string> lines;     // everything sent to the host's command port
        std::vector<uint8_t> received;      // everything sent to the host's data port
        int64_t busy_until = 0;             // keyed until
    };

    arq::Params params;
    std::mt19937 rng;
    double loss = 0.0;
    float snr = 25.0f;
    int decode_ms = 100;       // frame end to decoded frame
    bool model_busy = true;    // report channel busy to the receiver during each frame
    int64_t now = 0;
    uint64_t seq = 0;
    std::priority_queue<Ev, std::vector<Ev>, std::greater<Ev>> q;
    Station st[2];
    bool verbose = false;

    explicit Sim(uint32_t seed) : rng(seed) {
        for (int i = 0; i < 2; ++i) {
            VaraHost::Radio radio;
            radio.transmit = [this, i](const std::vector<std::vector<uint8_t>>& frames,
                                       const arq::ModeDesc& mode, bool) { transmit(i, frames, mode); };
            VaraHost::Host host;
            host.send_line = [this, i](const std::string& l) {
                st[i].lines.push_back(l);
                if (verbose) printf("  [%7.1fs] %c< %s\n", now / 1000.0, 'A' + i, l.c_str());
            };
            host.send_data = [this, i](const std::vector<uint8_t>& d) {
                st[i].received.insert(st[i].received.end(), d.begin(), d.end());
            };
            if (verbose)
                host.log = [this, i](const std::string& m) { printf("  [%7.1fs] %c  %s\n", now / 1000.0, 'A' + i, m.c_str()); };
            st[i].vh = std::make_unique<VaraHost>(params, std::move(radio), std::move(host), rng());
            st[i].vh->host_connected(0);
        }
    }

    void at(int64_t t, std::function<void()> fn) { q.push({t, seq++, std::move(fn)}); }

    void transmit(int i, const std::vector<std::vector<uint8_t>>& frames, const arq::ModeDesc& mode) {
        int64_t start = std::max(now, st[i].busy_until);
        at(start, [this, i] { st[i].vh->keyed(now); });
        int64_t t = start + params.key_lead_ms;
        for (const auto& f : frames) {
            // the peer's demodulator is synced for the whole frame
            if (model_busy) at(t, [this, i] { st[1 - i].vh->channel_busy(true); });
            t += mode.air_ms;
            bool lost = std::uniform_real_distribution<double>(0, 1)(rng) < loss;
            if (!lost) {
                auto bytes = f;
                int air = mode.air_ms;
                float s = snr;
                at(t + decode_ms, [this, i, bytes, air, s] { st[1 - i].vh->frame_received(bytes, s, air, now); });
            }
            if (model_busy) at(t + decode_ms, [this, i] { st[1 - i].vh->channel_busy(false); });
        }
        t += params.key_tail_ms;
        st[i].busy_until = t;
        at(t, [this, i] { st[i].vh->unkeyed(now); });
    }

    void cmd(int i, const std::string& line) {
        if (verbose) printf("  [%7.1fs] %c> %s\n", now / 1000.0, 'A' + i, line.c_str());
        st[i].vh->command(line, now);
    }

    // Run until `until` (simulated ms), ticking both stations every 10 ms
    void run(int64_t until) {
        while (now < until) {
            int64_t next = now + 10;
            while (!q.empty() && q.top().t <= next) {
                Ev e = q.top();
                q.pop();
                now = std::max(now, e.t);
                e.fn();
            }
            now = next;
            for (auto& s : st) s.vh->tick(now);
        }
    }

    // Run until pred() holds or `limit` ms pass; returns pred()
    bool run_until(std::function<bool()> pred, int64_t limit) {
        int64_t end = now + limit;
        while (now < end && !pred()) run(now + 10);
        return pred();
    }

    bool saw(int i, const std::string& prefix) const {
        for (const auto& l : st[i].lines)
            if (l.rfind(prefix, 0) == 0) return true;
        return false;
    }
};

static int fails = 0, checks = 0;

static void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++fails;
        printf("FAIL: %s\n", what.c_str());
    }
}

static std::vector<uint8_t> pattern(size_t n, uint32_t seed) {
    std::mt19937 r(seed);
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = (uint8_t)r();
    return v;
}

// One session as Pat would run it: A calls B, both sides send, A disconnects
static void session(uint32_t seed, double loss, size_t a_bytes, size_t b_bytes, bool verbose,
                    float snr = 25.0f, int decode_ms = 100, bool expect_top = false) {
    char tag[64];
    snprintf(tag, sizeof tag, "seed %u loss %.0f%% snr %.0f decode %d ms", seed, loss * 100, snr, decode_ms);
    Sim s(seed);
    s.loss = loss;
    s.snr = snr;
    s.decode_ms = decode_ms;
    s.verbose = verbose;

    s.cmd(0, "MYCALL N0AAA");
    s.cmd(0, "LISTEN ON");
    s.cmd(1, "MYCALL N0BBB N0BBB-1");
    s.cmd(1, "LISTEN ON");
    s.cmd(1, "COMPRESSION OFF");
    s.cmd(1, "BW2300");
    check(s.st[0].lines == std::vector<std::string>({"OK", "OK"}), std::string(tag) + ": A setup replies");
    check(s.st[1].lines == std::vector<std::string>({"OK", "OK", "OK", "OK"}), std::string(tag) + ": B setup replies");
    s.cmd(0, "FOO BAR");
    check(s.st[0].lines.back() == "WRONG", std::string(tag) + ": unknown command gets WRONG");
    s.cmd(0, "VERSION");
    check(s.st[0].lines.back().rfind("VERSION ", 0) == 0, std::string(tag) + ": VERSION reply");

    s.cmd(0, "CONNECT N0AAA N0BBB");
    check(s.st[0].lines.back() == "OK", std::string(tag) + ": CONNECT accepted");
    bool up = s.run_until([&] { return s.saw(0, "CONNECTED") && s.saw(1, "CONNECTED"); }, 120000);
    check(up, std::string(tag) + ": both sides CONNECTED");
    if (!up) return;
    check(s.saw(0, "CONNECTED N0AAA N0BBB NARROW"), std::string(tag) + ": caller's CONNECTED names caller first");
    check(s.saw(1, "CONNECTED N0AAA N0BBB NARROW"), std::string(tag) + ": called side's CONNECTED names caller first");
    check(s.saw(1, "PENDING"), std::string(tag) + ": called side saw PENDING");

    auto a_data = pattern(a_bytes, seed * 2 + 1);
    auto b_data = pattern(b_bytes, seed * 2 + 2);
    s.st[1].vh->data(b_data, s.now);   // the called side speaks first, as a Winlink gateway does
    s.st[0].vh->data(a_data, s.now);
    bool done = s.run_until([&] {
        return s.st[1].received.size() >= a_data.size() && s.st[0].received.size() >= b_data.size();
    }, 900000);
    check(done, std::string(tag) + ": all data delivered (A got " + std::to_string(s.st[0].received.size()) +
                    "/" + std::to_string(b_bytes) + ", B got " + std::to_string(s.st[1].received.size()) +
                    "/" + std::to_string(a_bytes) + ")");
    check(s.st[1].received == a_data, std::string(tag) + ": A->B data intact");
    check(s.st[0].received == b_data, std::string(tag) + ": B->A data intact");
    check(s.saw(0, "BUFFER") && s.saw(0, "PTT ON") && s.saw(0, "PTT OFF") && s.saw(0, "SN "),
          std::string(tag) + ": BUFFER, PTT and SN reports");
    check(s.saw(0, "BITRATE ("), std::string(tag) + ": BITRATE report");
    if (expect_top) {
        // a clean, strong channel reaches the top step and stays there
        const int top = (int)s.params.ladder_narrow.size() - 1;
        check(s.st[1].vh->link().rx_step() == top,
              std::string(tag) + ": B's receive step at the top (" + std::to_string(s.st[1].vh->link().rx_step()) + ")");
        check(s.saw(0, "BUSY ON") && s.saw(0, "BUSY OFF"), std::string(tag) + ": BUSY reports");
    }
    // BUFFER drains to 0 once the peer has acknowledged everything
    s.run_until([&] { return s.st[0].lines.back() == "BUFFER 0" || s.saw(0, "BUFFER 0"); }, 60000);
    check(s.saw(0, "BUFFER 0"), std::string(tag) + ": BUFFER drained to 0");

    s.cmd(0, "DISCONNECT");
    bool down = s.run_until([&] { return s.saw(0, "DISCONNECTED") && s.saw(1, "DISCONNECTED"); }, 120000);
    check(down, std::string(tag) + ": both sides DISCONNECTED");

    // Data from the host while disconnected is dropped, not queued for the next call
    size_t before = s.st[1].received.size();
    s.st[0].vh->data(pattern(100, 7), s.now);
    s.run(s.now + 20000);
    check(s.st[1].received.size() == before, std::string(tag) + ": no data sent while disconnected");

    // DISCONNECT while idle still answers DISCONNECTED, so a host never waits forever
    size_t n = s.st[0].lines.size();
    s.cmd(0, "DISCONNECT");
    check(s.st[0].lines.size() == n + 2 && s.st[0].lines[n + 1] == "DISCONNECTED",
          std::string(tag) + ": idle DISCONNECT answers DISCONNECTED");
}

// Calling a station that never answers ends in DISCONNECTED
static void no_answer(bool verbose) {
    Sim s(99);
    s.verbose = verbose;
    s.loss = 1.0;
    s.cmd(0, "MYCALL N0AAA");
    s.cmd(0, "CONNECT N0AAA N0ZZZ");
    bool gave_up = s.run_until([&] { return s.saw(0, "DISCONNECTED"); }, 300000);
    check(gave_up, "unanswered CONNECT ends in DISCONNECTED");
    check(!s.saw(0, "CONNECTED "), "unanswered CONNECT never reports CONNECTED");
}

// The host dropping its command connection mid-session ends the session on both sides
static void host_drops(bool verbose) {
    Sim s(5);
    s.verbose = verbose;
    s.cmd(0, "MYCALL N0AAA");
    s.cmd(1, "MYCALL N0BBB");
    s.cmd(1, "LISTEN ON");
    s.cmd(0, "CONNECT N0AAA N0BBB");
    s.run_until([&] { return s.saw(1, "CONNECTED"); }, 120000);
    s.st[0].vh->host_gone(s.now);
    bool b_down = s.run_until([&] { return s.saw(1, "DISCONNECTED"); }, 120000);
    check(b_down, "host drop: peer told DISCONNECTED");
}

// BW WIDE at both ends gives a wide session; narrow at either end falls back to narrow
static void wide_session(const char* bw_a, const char* bw_b, bool expect_wide, bool verbose) {
    char tag[64];
    snprintf(tag, sizeof tag, "A %s, B %s", bw_a, bw_b);
    Sim s(40);
    s.snr = 37.0f;
    s.verbose = verbose;
    s.cmd(0, "MYCALL N0AAA");
    s.cmd(0, bw_a);
    s.cmd(0, "BW2300");   // VARA HF widths leave the setting alone
    s.cmd(1, "MYCALL N0BBB");
    s.cmd(1, "LISTEN ON");
    s.cmd(1, bw_b);
    check(s.st[0].lines == std::vector<std::string>({"OK", "OK", "OK"}), std::string(tag) + ": A setup replies");
    s.cmd(0, "CONNECT N0AAA N0BBB");
    bool up = s.run_until([&] { return s.saw(0, "CONNECTED") && s.saw(1, "CONNECTED"); }, 120000);
    check(up, std::string(tag) + ": both sides CONNECTED");
    if (!up) return;
    const char* want = expect_wide ? "CONNECTED N0AAA N0BBB WIDE" : "CONNECTED N0AAA N0BBB NARROW";
    check(s.saw(0, want) && s.saw(1, want), std::string(tag) + ": " + want);
    auto a_data = pattern(120000, 41);
    s.st[0].vh->data(a_data, s.now);
    bool done = s.run_until([&] { return s.st[1].received.size() >= a_data.size(); }, 900000);
    check(done && s.st[1].received == a_data, std::string(tag) + ": data intact");
    const auto& top_ladder = expect_wide ? s.params.ladder_wide : s.params.ladder_narrow;
    check(s.st[1].vh->link().rx_step() == (int)top_ladder.size() - 1,
          std::string(tag) + ": receive step at the top of the " + (expect_wide ? "wide" : "narrow") + " ladder");
    s.cmd(0, "DISCONNECT");
    check(s.run_until([&] { return s.saw(0, "DISCONNECTED") && s.saw(1, "DISCONNECTED"); }, 120000),
          std::string(tag) + ": both sides DISCONNECTED");
}

int main(int argc, char** argv) {
    bool verbose = argc > 1 && std::string(argv[1]) == "-v";
    session(1, 0.0, 20000, 3000, verbose);
    session(2, 0.0, 300, 300, false);
    session(3, 0.10, 20000, 5000, false);
    session(4, 0.25, 8000, 8000, false);
    // strong channel, slow decoder: the receiver must not key up over a long QAM1024 frame
    session(6, 0.0, 120000, 2000, false, 37.0f, 1500, true);
    no_answer(false);
    host_drops(false);
    wide_session("BW WIDE", "BWWIDE", true, false);
    wide_session("BW WIDE", "BW NARROW", false, false);
    wide_session("BWNARROW", "BW WIDE", false, false);
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
