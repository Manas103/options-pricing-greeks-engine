// A simulated UDP multicast chain feed. This is a POD struct sent as raw
// bytes over a real UDP multicast socket on this host; it is not ITCH, not
// OPRA, and not any real exchange's wire format, just a minimal stand-in
// with the one property this extension's claims need: a sequence of ticks
// a quote engine receives one at a time, in order, over multicast, that can
// be captured and replayed bit for bit. See README "Honest framing".
#pragma once
#include <cstdint>
#include <cstring>

namespace quote {

constexpr const char* MCAST_GROUP = "239.255.0.1";
constexpr int MCAST_PORT = 30301;

constexpr uint64_t SENTINEL_SEQ = UINT64_MAX;

// Fixed layout, same-host only: this is not a portable wire format (no
// explicit byte order), sent as raw struct bytes between two processes on
// the same machine. A real cross-host feed would need an explicit,
// endianness-stable encoding; disclosed in README Limitations.
struct Tick {
    uint64_t seq;
    double spot;
    double sigma;
};

inline Tick make_sentinel() {
    Tick t{};
    t.seq = SENTINEL_SEQ;
    t.spot = 0.0;
    t.sigma = 0.0;
    return t;
}

} // namespace quote
