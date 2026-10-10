#pragma once

// ARQ frame encode/decode (Section 8.4). Stateless; multi-byte fields are big-endian.

#include <cstdint>
#include <string>
#include <vector>

namespace arq {

constexpr uint8_t MAGIC = 0xA7;
constexpr uint8_t VERSION = 1;
constexpr size_t HEADER_SIZE = 4;
constexpr size_t DATA_HEADER_SIZE = 10;
constexpr size_t MAX_CALL = 10;
constexpr size_t MAX_SACK = 3;

enum class FrameType : uint8_t {
    CONNECT = 1,
    CONNECT_ACK = 2,
    REJECT = 3,
    DATA = 4,
    ACK = 5,
    POLL = 6,
    TURN = 7,
    DISC = 8,
    DISC_ACK = 9,
};

enum class RejectReason : uint8_t { BUSY = 1, NOT_LISTENING = 2, BAD_PARAMS = 3 };
enum class DiscReason : uint8_t { HOST = 1, ABORT = 2, LINK_FAILURE = 3 };

// DATA.flags
constexpr uint8_t DATA_LAST = 0x01;
constexpr int DATA_BURST_SHIFT = 4;        // bits 4-6: burst size - 1
// ACK.flags
constexpr uint8_t ACK_HAVE_DATA = 0x01;
constexpr uint8_t ACK_DUPLICATE = 0x02;
// POLL.flags
constexpr uint8_t POLL_KEEPALIVE = 0x01;
constexpr uint8_t POLL_TURN_REQUEST = 0x02;

struct SackRange {
    uint32_t start = 0;
    uint16_t len = 0;
};

// Decoded frame; only the fields of `type` are meaningful.
struct Frame {
    FrameType type = FrameType::DATA;
    uint16_t session = 0;
    uint8_t flags = 0;
    // CONNECT / CONNECT_ACK
    uint8_t burst = 0;
    uint8_t window_kib = 0;
    std::string src, dst;
    int8_t snr = 0;
    // REJECT / DISC
    uint8_t reason = 0;
    // DATA
    uint32_t offset = 0;
    uint8_t remaining = 0;
    std::vector<uint8_t> payload;
    // ACK
    uint32_t cum = 0;
    uint8_t step = 0;
    uint8_t rx_space_kib = 0;
    std::vector<SackRange> sack;
    // TURN: next byte of the peer's stream we expect
    uint32_t next_expected = 0;
};

namespace detail {
inline void put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(v >> 8);
    b.push_back(v & 0xFF);
}
inline void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back((v >> s) & 0xFF);
}
inline uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
inline uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
inline void put_call(std::vector<uint8_t>& b, const std::string& c) {
    size_t n = c.size() < MAX_CALL ? c.size() : MAX_CALL;
    b.push_back((uint8_t)n);
    b.insert(b.end(), c.begin(), c.begin() + n);
}
}  // namespace detail

inline std::vector<uint8_t> encode(const Frame& f) {
    using namespace detail;
    std::vector<uint8_t> b;
    b.push_back(MAGIC);
    b.push_back((uint8_t)(VERSION << 4 | (uint8_t)f.type));
    put16(b, f.session);
    switch (f.type) {
    case FrameType::CONNECT:
        b.push_back(f.flags);
        b.push_back(f.burst);
        b.push_back(f.window_kib);
        put_call(b, f.src);
        put_call(b, f.dst);
        break;
    case FrameType::CONNECT_ACK:
        b.push_back(f.flags);
        b.push_back(f.burst);
        b.push_back(f.window_kib);
        b.push_back((uint8_t)f.snr);
        break;
    case FrameType::REJECT:
    case FrameType::DISC:
        b.push_back(f.reason);
        break;
    case FrameType::DATA:
        put32(b, f.offset);
        b.push_back(f.remaining);
        b.push_back(f.flags);
        b.insert(b.end(), f.payload.begin(), f.payload.end());
        break;
    case FrameType::ACK: {
        put32(b, f.cum);
        b.push_back((uint8_t)f.snr);
        b.push_back(f.step);
        b.push_back(f.flags);
        b.push_back(f.rx_space_kib);
        size_t n = f.sack.size() < MAX_SACK ? f.sack.size() : MAX_SACK;
        b.push_back((uint8_t)n);
        for (size_t i = 0; i < n; ++i) {
            put32(b, f.sack[i].start);
            put16(b, f.sack[i].len);
        }
        break;
    }
    case FrameType::POLL:
        b.push_back(f.flags);
        break;
    case FrameType::TURN:
        put32(b, f.next_expected);
        break;
    case FrameType::DISC_ACK:
        break;
    }
    return b;
}

// Returns false for anything that is not a well-formed ARQ frame of this version.
inline bool decode(const uint8_t* p, size_t n, Frame& f) {
    using namespace detail;
    if (n < HEADER_SIZE || p[0] != MAGIC || (p[1] >> 4) != VERSION) return false;
    uint8_t t = p[1] & 0x0F;
    if (t < 1 || t > 9) return false;
    f = Frame{};
    f.type = (FrameType)t;
    f.session = get16(p + 2);
    const uint8_t* q = p + HEADER_SIZE;
    size_t left = n - HEADER_SIZE;
    auto need = [&](size_t k) { return left >= k; };
    auto take_call = [&](std::string& out) {
        if (!need(1)) return false;
        size_t len = q[0];
        if (len > MAX_CALL || !need(1 + len)) return false;
        out.assign((const char*)q + 1, len);
        q += 1 + len;
        left -= 1 + len;
        return true;
    };
    switch (f.type) {
    case FrameType::CONNECT:
        if (!need(3)) return false;
        f.flags = q[0];
        f.burst = q[1];
        f.window_kib = q[2];
        q += 3;
        left -= 3;
        return take_call(f.src) && take_call(f.dst);
    case FrameType::CONNECT_ACK:
        if (!need(4)) return false;
        f.flags = q[0];
        f.burst = q[1];
        f.window_kib = q[2];
        f.snr = (int8_t)q[3];
        return true;
    case FrameType::REJECT:
    case FrameType::DISC:
        if (!need(1)) return false;
        f.reason = q[0];
        return true;
    case FrameType::DATA:
        if (!need(6)) return false;
        f.offset = get32(q);
        f.remaining = q[4];
        f.flags = q[5];
        f.payload.assign(q + 6, q + left);
        return true;
    case FrameType::ACK: {
        if (!need(9)) return false;
        f.cum = get32(q);
        f.snr = (int8_t)q[4];
        f.step = q[5];
        f.flags = q[6];
        f.rx_space_kib = q[7];
        size_t cnt = q[8];
        if (cnt > MAX_SACK || !need(9 + cnt * 6)) return false;
        for (size_t i = 0; i < cnt; ++i) {
            const uint8_t* r = q + 9 + i * 6;
            f.sack.push_back({get32(r), get16(r + 4)});
        }
        return true;
    }
    case FrameType::POLL:
        if (!need(1)) return false;
        f.flags = q[0];
        return true;
    case FrameType::TURN:
        if (!need(4)) return false;
        f.next_expected = get32(q);
        return true;
    case FrameType::DISC_ACK:
        return true;
    }
    return false;
}

}  // namespace arq
