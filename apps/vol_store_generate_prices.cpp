// Computes the "true" American price for every (underlying, day, expiry,
// strike) combination the Python simulator laid out, using the exact same
// CRR engine and step count the batch inversion later uses. Python adds
// bid/ask noise on top of this price to form the simulated EOD quote; the
// point of sharing the pricer is that the American inversion's job is then
// to recover a known sigma_true (modulo tree discretization and noise),
// which is what makes the European-vs-American vol-point gap a measured
// quantity instead of an assumption.
//
// Input/output share the QuoteRecord layout vol_store_invert.cpp uses;
// mid_price is an input placeholder (ignored) and is OVERWRITTEN with the
// computed true price on output.
//
// Usage: ./vol_store_generate_prices <params.bin> <divs.csv> <out.bin> <threads> <tree_steps>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "american_tree.hpp"

#pragma pack(push, 1)
struct QuoteRecord {
    int32_t underlying_id;
    int32_t day_index;
    int32_t expiry_day_index;
    int32_t is_put;
    float strike;
    float S0;
    float r;
    float borrow;
    float mid_price;
    float sigma_true;
};
#pragma pack(pop)
static_assert(sizeof(QuoteRecord) == 40, "layout must match export_binary.py");

using DivMap = std::map<int32_t, std::vector<std::pair<int32_t, double>>>;

static DivMap load_dividends(const std::string& path) {
    DivMap m;
    std::ifstream f(path);
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string a, b, c;
        std::getline(ss, a, ',');
        std::getline(ss, b, ',');
        std::getline(ss, c, ',');
        m[std::stoi(a)].emplace_back(std::stoi(b), std::stod(c));
    }
    for (auto& kv : m) std::sort(kv.second.begin(), kv.second.end());
    return m;
}

static void worker(std::vector<QuoteRecord>& quotes, size_t begin, size_t end,
                    const DivMap& divmap, int tree_steps) {
    crr::Workspace ws(tree_steps);
    for (size_t i = begin; i < end; ++i) {
        QuoteRecord& q = quotes[i];
        double T = static_cast<double>(q.expiry_day_index - q.day_index) / 252.0;
        std::vector<crr::DiscreteDividend> divs;
        auto it = divmap.find(q.underlying_id);
        if (it != divmap.end()) {
            for (auto& dv : it->second) {
                if (dv.first > q.day_index && dv.first <= q.expiry_day_index) {
                    divs.push_back({static_cast<double>(dv.first - q.day_index) / 252.0, dv.second});
                }
            }
        }
        bool is_call = q.is_put == 0;
        crr::TreeParams p{q.S0, q.strike, q.r, q.borrow, q.sigma_true, T, tree_steps, &divs};
        double price = ws.price(p, is_call, crr::Exercise::American);
        q.mid_price = static_cast<float>(price);
    }
}

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: %s <params.bin> <divs.csv> <out.bin> <threads> <tree_steps>\n",
                      argv[0]);
        return 1;
    }
    std::string in_path = argv[1], divs_path = argv[2], out_path = argv[3];
    int nthreads = std::atoi(argv[4]);
    int tree_steps = std::atoi(argv[5]);

    std::ifstream in(in_path, std::ios::binary);
    in.seekg(0, std::ios::end);
    std::streamoff bytes = in.tellg();
    in.seekg(0, std::ios::beg);
    size_t n = static_cast<size_t>(bytes) / sizeof(QuoteRecord);
    std::vector<QuoteRecord> quotes(n);
    in.read(reinterpret_cast<char*>(quotes.data()), bytes);
    in.close();

    DivMap divmap = load_dividends(divs_path);

    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    size_t chunk = (n + static_cast<size_t>(nthreads) - 1) / static_cast<size_t>(nthreads);
    for (int t = 0; t < nthreads; ++t) {
        size_t begin = static_cast<size_t>(t) * chunk;
        size_t end = std::min(n, begin + chunk);
        if (begin >= end) continue;
        threads.emplace_back(worker, std::ref(quotes), begin, end, std::cref(divmap), tree_steps);
    }
    for (auto& th : threads) th.join();
    auto t1 = std::chrono::high_resolution_clock::now();
    double seconds = std::chrono::duration<double>(t1 - t0).count();

    std::ofstream out(out_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(quotes.data()),
              static_cast<std::streamsize>(quotes.size() * sizeof(QuoteRecord)));
    out.close();

    std::printf("vol_store_generate_prices: %zu rows, %d threads, tree_steps=%d, %.3f s (%.0f rows/sec)\n",
                n, nthreads, tree_steps, seconds, static_cast<double>(n) / seconds);
    return 0;
}
