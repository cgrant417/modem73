#pragma once

#include <cstddef>
#include <cstdint>

// Transmitter activity, reported through KISSTNC::tx_event_callback on the TX thread.
// A transmission is KEYED, one FRAME_DONE per frame, then UNKEYED.
struct TxEvent {
    enum class Type { KEYED, FRAME_DONE, UNKEYED };
    Type type = Type::KEYED;
    size_t bytes = 0;      // FRAME_DONE: payload bytes in the frame
    bool ok = true;        // FRAME_DONE: false if the frame was dropped (too large or encode failed)
    bool beacon = false;   // presence tone rather than a data frame
    int64_t steady_ms = 0;
};

inline const char* tx_event_name(TxEvent::Type t) {
    switch (t) {
    case TxEvent::Type::KEYED: return "keyed";
    case TxEvent::Type::FRAME_DONE: return "frame_done";
    case TxEvent::Type::UNKEYED: return "unkeyed";
    }
    return "?";
}
