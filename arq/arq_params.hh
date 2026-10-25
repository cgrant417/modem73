#pragma once

// ARQ link parameters: timers, burst/window limits and the AMC mode ladders.
// See ofdm-fm-design-proposal.md Section 8 (8.5, 8.7, 8.8).

#include <cstdint>
#include <string>
#include <vector>

namespace arq {

// A PHY mode as the ARQ engine sees it. The integration maps `name` to a modem73 oper_mode.
struct ModeDesc {
    std::string name;
    int payload = 0;     // usable bytes per frame (after modem73's 2-byte length prefix)
    int air_ms = 0;      // airtime of a full frame, excluding PTT/TXDelay
};

// Bandwidth classes, as bits in CONNECT / CONNECT_ACK flags (Section 8.4)
constexpr uint8_t BW_NARROW = 0x01;
constexpr uint8_t BW_WIDE = 0x02;
constexpr uint8_t BW_MASK = 0x03;

struct LadderStep {
    ModeDesc mode;
    int min_snr_db;      // lowest burst-mean S/N (the decoder's estimate) this step is used at
};

// Narrow (2.4 kHz) ladder, Section 8.8; payload/airtime measured in Section 8.1.
// min_snr_db is about 1 dB above where the step stops decoding every frame, measured as
// the decoder's mean S/N on cable recordings with added noise (design proposal 7.8):
// 1.9, 2.6, 4.8, 7.1, 10.3, 19.1, 22.9, 25.6, 27.8 dB. Not yet checked on air.
inline std::vector<LadderStep> narrow_ladder() {
    return {
        {{"BPSK 1/2 normal", 254, 2733}, -100},
        {{"QPSK 1/2 normal", 510, 2733}, 4},
        {{"QPSK 3/4 normal", 766, 2733}, 6},
        {{"QAM16 1/2 normal", 1022, 2733}, 8},
        {{"QAM16 3/4 normal", 1534, 2733}, 11},
        {{"QAM256 2/3 normal", 2734, 2733}, 20},
        {{"QAM256 5/6 normal", 3406, 2733}, 24},
        {{"QAM1024 3/4 normal", 6142, 4100}, 27},
        // 2 dB above its edge: on the cable alone (about 29 dB) it lost 1-2 frames in 8
        {{"QAM1024 5/6 normal", 6814, 4100}, 30},
    };
}

struct Params {
    // Timers (Section 8.7)
    int turnaround_ms = 300;          // G: wait after the peer's transmission before keying
    int key_lead_ms = 550;            // PTT delay + TXDelay before the first frame's audio
    int key_tail_ms = 50;             // PTT tail after the last frame
    int decode_margin_ms = 300;
    int ack_margin_ms = 1000;
    int burst_end_margin_ms = 1000;   // IRS: slack after the expected end of a burst
    int poll_retries = 5;
    int connect_retry_min_ms = 4000;
    int connect_retry_max_ms = 6000;
    int connect_attempts = 8;
    int idle_poll_min_ms = 3000;
    int idle_poll_max_ms = 30000;
    int link_timeout_ms = 90000;
    int disc_attempts = 3;
    int last_session_hold_ms = 30000; // keep answering a closed session's DISC this long

    // Data flow (Section 8.3, 8.6)
    int burst_max = 8;                // B, 1..8
    int burst_air_max_ms = 24000;     // a burst holds at most this much frame airtime
    int window_bytes = 64 * 1024;     // W
    int turn_fairness_bursts = 4;

    // Modes (Section 8.5, 8.8)
    ModeDesc connect_mode{"BPSK 1/2 short", 126, 1640};
    ModeDesc control_mode{"QPSK 1/2 short", 126, 1093};
    std::vector<LadderStep> ladder_narrow = narrow_ladder();
    std::vector<LadderStep> ladder_wide;   // defined in Phase 5 from measured wide modes
    int start_step = 1;
    int up_after_clean_bursts = 3;    // after a lossy burst; before any loss one clean burst may jump to the SNR's step
    int up_snr_margin_db = 2;

    uint8_t bw_offer = BW_NARROW;     // bandwidth classes offered/accepted (host BW command)

    const std::vector<LadderStep>& ladder(uint8_t bw) const {
        return (bw == BW_WIDE && !ladder_wide.empty()) ? ladder_wide : ladder_narrow;
    }

    // Our unkeyed event to giving up on a reply (Section 8.7)
    int ack_timeout_ms() const {
        return turnaround_ms + key_lead_ms + key_tail_ms + control_mode.air_ms +
               decode_margin_ms + ack_margin_ms;
    }

    // After TURN the reply is the new ISS's first DATA frame, which can be the slowest mode
    int turn_timeout_ms() const {
        int air = control_mode.air_ms;
        for (const auto* l : {&ladder_narrow, &ladder_wide})
            for (const auto& s : *l) air = air > s.mode.air_ms ? air : s.mode.air_ms;
        return turnaround_ms + key_lead_ms + key_tail_ms + air + decode_margin_ms + ack_margin_ms;
    }
};

}  // namespace arq
