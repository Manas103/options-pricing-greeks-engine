// Simulated UDP multicast chain feed sender. Sends n_ticks ticks (a GBM-ish
// random walk in spot, a small independent walk in a flat vol shock) to
// the multicast group quote_engine_live listens on, paced at tick_us
// microseconds apart, then sends the sentinel tick 20 times (UDP has no
// delivery guarantee; repeating the sentinel costs nothing and makes a
// dropped end-of-session signal astronomically unlikely on a loopback
// path). Exits on its own once the sentinels are sent; nothing is left
// running.
//
// Usage: quote_feed_sender <n_ticks> <tick_us> <seed>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include "quote_chain.hpp"
#include "tick_feed.hpp"

int main(int argc, char** argv) {
    long n_ticks = (argc > 1) ? std::atol(argv[1]) : 50000;
    long tick_us = (argc > 2) ? std::atol(argv[2]) : 50;
    unsigned seed = (argc > 3) ? static_cast<unsigned>(std::atoi(argv[3])) : 2027u;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::perror("socket");
        return 1;
    }
    unsigned char ttl = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    unsigned char loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(quote::MCAST_PORT);
    dest.sin_addr.s_addr = inet_addr(quote::MCAST_GROUP);

    quote::ChainSpec spec{};
    std::mt19937 rng(seed);
    std::normal_distribution<double> spot_shock(0.0, 0.0015); // ~ per-tick log-return
    std::normal_distribution<double> vol_shock(0.0, 0.0008);

    double S = spec.S0;
    double sigma = spec.base_sigma;

    std::printf("sender: multicast group %s:%d, %ld ticks, %ld us apart, seed=%u\n",
                quote::MCAST_GROUP, quote::MCAST_PORT, n_ticks, tick_us, seed);

    for (long i = 0; i < n_ticks; ++i) {
        S *= std::exp(spot_shock(rng));
        sigma += vol_shock(rng);
        if (sigma < 0.05) sigma = 0.05; // keep the chain's vol input sane
        if (sigma > 0.80) sigma = 0.80;

        quote::Tick t{};
        t.seq = static_cast<uint64_t>(i);
        t.spot = S;
        t.sigma = sigma;

        ssize_t sent = sendto(sock, &t, sizeof(t), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        if (sent != static_cast<ssize_t>(sizeof(t))) {
            std::perror("sendto");
        }
        if (tick_us > 0) usleep(static_cast<useconds_t>(tick_us));
    }

    quote::Tick sentinel = quote::make_sentinel();
    for (int i = 0; i < 20; ++i) {
        sendto(sock, &sentinel, sizeof(sentinel), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        usleep(2000);
    }

    close(sock);
    std::printf("sender: done, sent %ld ticks + 20 sentinels\n", n_ticks);
    return 0;
}
