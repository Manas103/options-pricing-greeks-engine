// The live quote engine: joins the multicast group, pins itself to one
// CPU core, and for every tick received reprices the full chain
// (quote_chain.hpp / quote_engine.hpp), timing strictly from "the datagram
// is in hand" to "every contract's quote has been written" with nothing
// else (no capture write, no stdout) inside that window. Capture and the
// running FNV-1a checksum happen after the timed window per tick, into
// buffers preallocated before the loop starts, so neither contributes
// allocator or I/O jitter to the measured latency.
//
// Exits on its own once the sentinel tick arrives; nothing is left
// running afterward.
//
// Usage: quote_engine_live <out_capture_path> <out_report_path> [pinned_core]
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "fnv1a.hpp"
#include "quote_chain.hpp"
#include "quote_engine.hpp"
#include "tick_feed.hpp"

namespace {

long long now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

bool pin_to_core(int core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <out_capture_path> <out_report_path> [pinned_core]\n", argv[0]);
        return 1;
    }
    std::string capture_path = argv[1];
    std::string report_path = argv[2];
    int n_cores = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    int core = (argc > 3) ? std::atoi(argv[3]) : std::max(0, n_cores - 1);

    bool pinned = pin_to_core(core);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::perror("socket");
        return 1;
    }
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(quote::MCAST_PORT);
    if (bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        std::perror("bind");
        return 1;
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(quote::MCAST_GROUP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        std::perror("IP_ADD_MEMBERSHIP");
        return 1;
    }

    quote::ChainSpec spec{};
    std::vector<quote::Pair> chain = quote::build_chain(spec);
    std::vector<quote::Quote> quotes(chain.size() * 2);

    constexpr long MAX_TICKS = 2000000;
    std::vector<long long> latencies_ns;
    latencies_ns.reserve(MAX_TICKS);
    std::vector<quote::Tick> captured;
    captured.reserve(MAX_TICKS);

    uint64_t checksum = quote::FNV_OFFSET_BASIS;
    quote::Tick tick{};

    std::printf("quote_engine_live: pinned_core=%d (pin %s), pairs=%zu (%zu contracts), listening on %s:%d\n",
                core, pinned ? "ok" : "FAILED", chain.size(), chain.size() * 2, quote::MCAST_GROUP, quote::MCAST_PORT);

    long n_received = 0;
    for (;;) {
        ssize_t got = recvfrom(sock, &tick, sizeof(tick), 0, nullptr, nullptr);
        if (got != static_cast<ssize_t>(sizeof(tick))) continue; // short/garbled datagram, ignore
        if (tick.seq == quote::SENTINEL_SEQ) break;

        long long t0 = now_ns();
        quote::reprice_chain(chain, tick.spot, spec.r, tick.sigma, quotes.data());
        long long t1 = now_ns();

        latencies_ns.push_back(t1 - t0);
        captured.push_back(tick);
        quote::fnv1a_fold(checksum, quotes.data(), quotes.size() * sizeof(quote::Quote));
        ++n_received;
    }

    close(sock);

    std::vector<long long> sorted_lat = latencies_ns;
    std::sort(sorted_lat.begin(), sorted_lat.end());
    auto pct = [&](double p) -> long long {
        if (sorted_lat.empty()) return -1;
        std::size_t idx = static_cast<std::size_t>(p * (sorted_lat.size() - 1));
        return sorted_lat[idx];
    };
    long long p50 = pct(0.50);
    long long p99 = pct(0.99);
    long long pmax = sorted_lat.empty() ? -1 : sorted_lat.back();
    long long pmin = sorted_lat.empty() ? -1 : sorted_lat.front();

    // Write the capture: header (tick count) then every received tick's raw
    // bytes, in receipt order. quote_engine_replay.cpp reads this back and
    // must reproduce the identical checksum printed below.
    {
        std::ofstream cap(capture_path, std::ios::binary | std::ios::trunc);
        uint64_t n = static_cast<uint64_t>(captured.size());
        cap.write(reinterpret_cast<const char*>(&n), sizeof(n));
        cap.write(reinterpret_cast<const char*>(captured.data()),
                  static_cast<std::streamsize>(captured.size() * sizeof(quote::Tick)));
    }

    std::ofstream rep(report_path, std::ios::trunc);
    auto emit = [&](std::ostream& os) {
        os << "quote_engine_live report\n";
        os << "pinned_core=" << core << " pin_ok=" << (pinned ? "true" : "false") << "\n";
        os << "contracts_per_tick=" << (chain.size() * 2) << "\n";
        os << "ticks_received=" << n_received << "\n";
        os << "tick_to_quote_ns: min=" << pmin << " p50=" << p50 << " p99=" << p99 << " max=" << pmax
           << "\n";
        os << "tick_to_quote_us: p50=" << (p50 / 1000.0) << " p99=" << (p99 / 1000.0) << "\n";
        os << "checksum_fnv1a64=0x" << std::hex << checksum << std::dec << "\n";
    };
    emit(rep);
    emit(std::cout);

    return 0;
}
