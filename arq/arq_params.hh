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
    int min_snr_db;      // lowest SNR this step is used at (placeholder until Phase 8 calibration)
};

// Narrow (2.4 kHz) ladder, Section 8.8; payload/airtime measured in Section 8.1
inline std::vector<LadderStep> narrow_ladder() {
    return {
        {{"BPSK 1/2 normal", 254, 2733}, -100},
        {{"QPSK 1/2 normal", 510, 2733}, 6},
        {{"QPSK 3/4 normal", 766, 2733}, 9},
        {{"QAM16 1/2 normal", 1022, 2733}, 12},
        {{"QAM16 3/4 normal", 1534, 2733}, 16},
        {{"QAM256 2/3 normal", 2734, 2733}, 22},
        {{"QAM256 5/6 normal", 3406, 2733}, 26},
        // Clean on a Digirig-to-Digirig cable (about 35 dB, Section 7.2); thresholds unverified on air
        {{"QAM1024 3/4 normal", 6142, 4100}, 30},
        {{"QAM1024 5/6 normal", 6814, 4100}, 33},
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
    int burst_max = 4;                // B, 1..8
    int window_bytes = 64 * 1024;     // W
    int turn_fairness_bursts = 4;

    // Modes (Section 8.5, 8.8)
    ModeDesc connect_mode{"BPSK 1/2 short", 126, 1640};
    ModeDesc control_mode{"QPSK 1/2 short", 126, 1093};
    std::vector<LadderStep> ladder_narrow = narrow_ladder();
    std::vector<LadderStep> ladder_wide;   // defined in Phase 5 from measured wide modes
    int start_step = 1;
    int up_after_clean_bursts = 3;
    int up_snr_margin_db = 3;

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
