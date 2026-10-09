#pragma once

#include <cstddef>
#include <cstdint>

// Transmitter activity, reported through KISSTNC::tx_event_callback on the TX thread.
// A burst is KEY_UP, one FRAME_DONE per frame, then KEY_DOWN.
struct TxEvent {
    enum class Type { KEY_UP, FRAME_DONE, KEY_DOWN };
    Type type = Type::KEY_UP;
    size_t bytes = 0;      // FRAME_DONE: payload bytes in the frame
    bool ok = true;        // FRAME_DONE: false if the frame was dropped (too large or encode failed)
    bool beacon = false;   // presence tone rather than a data frame
    int64_t steady_ms = 0;
};

inline const char* tx_event_name(TxEvent::Type t) {
    switch (t) {
    case TxEvent::Type::KEY_UP: return "key_up";
    case TxEvent::Type::FRAME_DONE: return "frame_done";
    case TxEvent::Type::KEY_DOWN: return "key_down";
    }
    return "?";
}
