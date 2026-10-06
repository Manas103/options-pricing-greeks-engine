// Offline replay: reads back exactly what quote_engine_live captured (no
// network, no timing), reprices the identical chain through the identical
// function for every tick in the identical order, and prints the FNV-1a
// checksum over the resulting quote stream. "Every session replayable bit
// for bit" means this checksum equals the one quote_engine_live printed
// for the same capture file; nothing here recomputes or re-derives
// anything, it replays recorded bytes.
//
// Usage: quote_engine_replay <capture_path> <out_report_path>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "fnv1a.hpp"
#include "quote_chain.hpp"
#include "quote_engine.hpp"
#include "tick_feed.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <capture_path> <out_report_path>\n", argv[0]);
        return 1;
    }
    std::string capture_path = argv[1];
    std::string report_path = argv[2];

    std::ifstream cap(capture_path, std::ios::binary);
    if (!cap) {
        std::fprintf(stderr, "could not open capture file: %s\n", capture_path.c_str());
        return 1;
    }
    uint64_t n = 0;
    cap.read(reinterpret_cast<char*>(&n), sizeof(n));
    std::vector<quote::Tick> ticks(n);
    cap.read(reinterpret_cast<char*>(ticks.data()), static_cast<std::streamsize>(n * sizeof(quote::Tick)));
    if (!cap) {
        std::fprintf(stderr, "capture file truncated or unreadable: %s\n", capture_path.c_str());
        return 1;
    }

    quote::ChainSpec spec{};
    std::vector<quote::Pair> chain = quote::build_chain(spec);
    std::vector<quote::Quote> quotes(chain.size() * 2);

    uint64_t checksum = quote::FNV_OFFSET_BASIS;
    for (const quote::Tick& t : ticks) {
        quote::reprice_chain(chain, t.spot, spec.r, t.sigma, quotes.data());
        quote::fnv1a_fold(checksum, quotes.data(), quotes.size() * sizeof(quote::Quote));
    }

    std::ofstream rep(report_path, std::ios::trunc);
    auto emit = [&](std::ostream& os) {
        os << "quote_engine_replay report\n";
        os << "capture_path=" << capture_path << "\n";
        os << "ticks_replayed=" << n << "\n";
        os << "contracts_per_tick=" << (chain.size() * 2) << "\n";
        os << "checksum_fnv1a64=0x" << std::hex << checksum << std::dec << "\n";
    };
    emit(rep);
    emit(std::cout);

    return 0;
}
