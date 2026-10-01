// Batch-inverts every quarantine-cleared quote in the single-stock vol
// store two ways: (1) the American CRR tree in american_tree.hpp, carrying
// the quote's own discrete dollar dividends and borrow rate, and (2) the
// plain closed-form Black-Scholes bisection already in implied_vol.hpp,
// which (by the original repo's own design, see README "No dividend
// yield") knows nothing about dividends or early exercise at all. The gap
// between the two, not either number alone, is the point of this
// extension.
//
// Reads a flat binary quote file (see python/vol_store/export_binary.py for
// the exact record layout this expects) and a small dividend-schedule CSV,
// and writes a flat binary results file plus a short human-readable summary
// to stdout. Multithreaded with std::thread, one contiguous row range per
// thread; thread count is a CLI argument so the benchmark run can document
// exactly how many threads it used and why (see README).
//
// Usage: ./vol_store_invert <quotes.bin> <divs.csv> <out.bin> <threads> <tree_steps> <r>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "american_tree.hpp"
#include "black_scholes.hpp"
#include "implied_vol.hpp"

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
struct ResultRecord {
    int32_t underlying_id;
    int32_t day_index;
    int32_t expiry_day_index;
    int32_t is_put;
    float strike;
    float S0;
    float sigma_true;
    float american_iv;
    int32_t american_ok;
    float european_iv;
    int32_t european_ok;
};
#pragma pack(pop)

static_assert(sizeof(QuoteRecord) == 40, "QuoteRecord layout must match export_binary.py exactly");
static_assert(sizeof(ResultRecord) == 44, "ResultRecord layout must match analyze_results.py exactly");

using DivMap = std::map<int32_t, std::vector<std::pair<int32_t, double>>>;

static DivMap load_dividends(const std::string& path) {
    DivMap m;
    std::ifstream f(path);
    std::string line;
    std::getline(f, line); // header
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string a, b, c;
        std::getline(ss, a, ',');
        std::getline(ss, b, ',');
        std::getline(ss, c, ',');
        int32_t uid = std::stoi(a);
        int32_t day = std::stoi(b);
        double amt = std::stod(c);
        m[uid].emplace_back(day, amt);
    }
    for (auto& kv : m) {
        std::sort(kv.second.begin(), kv.second.end());
    }
    return m;
}

struct ThreadStats {
    long american_ok = 0, american_fail = 0;
    long european_ok = 0, european_fail = 0;
};

static void worker(const std::vector<QuoteRecord>& quotes, size_t begin, size_t end,
                    const DivMap& divmap, int tree_steps, std::vector<ResultRecord>& out,
                    ThreadStats& stats) {
    crr::Workspace ws(tree_steps);
    for (size_t i = begin; i < end; ++i) {
        const QuoteRecord& q = quotes[i];
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
        crr::TreeParams p{q.S0, q.strike, q.r, q.borrow, 0.2, T, tree_steps, &divs};
        crr::ImpliedVolResult am =
            crr::solve_implied_vol(ws, p, is_call, crr::Exercise::American, q.mid_price);

        ivol::ImpliedVolResult eu =
            ivol::solve(is_call ? mc::OptionType::Call : mc::OptionType::Put, q.mid_price, q.S0,
                        q.strike, q.r, T);

        ResultRecord r{};
        r.underlying_id = q.underlying_id;
        r.day_index = q.day_index;
        r.expiry_day_index = q.expiry_day_index;
        r.is_put = q.is_put;
        r.strike = q.strike;
        r.S0 = q.S0;
        r.sigma_true = q.sigma_true;
        r.american_iv = static_cast<float>(am.vol);
        r.american_ok = am.converged ? 1 : 0;
        r.european_iv = static_cast<float>(eu.vol);
        r.european_ok = eu.converged ? 1 : 0;
        out[i] = r;

        if (am.converged) ++stats.american_ok; else ++stats.american_fail;
        if (eu.converged) ++stats.european_ok; else ++stats.european_fail;
    }
}

int main(int argc, char** argv) {
    if (argc < 7) {
        std::fprintf(stderr,
                      "usage: %s <quotes.bin> <divs.csv> <out.bin> <threads> <tree_steps> <r>\n",
                      argv[0]);
        return 1;
    }
    std::string quotes_path = argv[1];
    std::string divs_path = argv[2];
    std::string out_path = argv[3];
    int nthreads = std::atoi(argv[4]);
    int tree_steps = std::atoi(argv[5]);
    double r_unused = std::atof(argv[6]); // r is carried per-quote in the record; kept for logging
    (void)r_unused;

    std::ifstream in(quotes_path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", quotes_path.c_str());
        return 1;
    }
    in.seekg(0, std::ios::end);
    std::streamoff bytes = in.tellg();
    in.seekg(0, std::ios::beg);
    size_t n = static_cast<size_t>(bytes) / sizeof(QuoteRecord);
    std::vector<QuoteRecord> quotes(n);
    in.read(reinterpret_cast<char*>(quotes.data()), bytes);
    in.close();

    DivMap divmap = load_dividends(divs_path);

    std::vector<ResultRecord> results(n);
    std::vector<ThreadStats> stats(static_cast<size_t>(nthreads));
    std::vector<std::thread> threads;
    size_t chunk = (n + static_cast<size_t>(nthreads) - 1) / static_cast<size_t>(nthreads);

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int t = 0; t < nthreads; ++t) {
        size_t begin = static_cast<size_t>(t) * chunk;
        size_t end = std::min(n, begin + chunk);
        if (begin >= end) continue;
        threads.emplace_back(worker, std::cref(quotes), begin, end, std::cref(divmap), tree_steps,
                              std::ref(results), std::ref(stats[static_cast<size_t>(t)]));
    }
    for (auto& th : threads) th.join();
    auto t1 = std::chrono::high_resolution_clock::now();
    double seconds = std::chrono::duration<double>(t1 - t0).count();

    std::ofstream out(out_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(results.data()),
              static_cast<std::streamsize>(results.size() * sizeof(ResultRecord)));
    out.close();

    long am_ok = 0, am_fail = 0, eu_ok = 0, eu_fail = 0;
    for (auto& s : stats) {
        am_ok += s.american_ok;
        am_fail += s.american_fail;
        eu_ok += s.european_ok;
        eu_fail += s.european_fail;
    }

    std::printf("vol_store_invert: %zu quotes, %d threads, tree_steps=%d\n", n, nthreads,
                tree_steps);
    std::printf("wall time: %.3f s  (%.0f quotes/sec)\n", seconds,
                static_cast<double>(n) / seconds);
    std::printf("american inversion:  %ld converged, %ld failed to bracket (%.4f%%)\n", am_ok,
                am_fail, 100.0 * static_cast<double>(am_fail) / static_cast<double>(n));
    std::printf("european inversion:  %ld converged, %ld failed to bracket (%.4f%%)\n", eu_ok,
                eu_fail, 100.0 * static_cast<double>(eu_fail) / static_cast<double>(n));
    std::printf("results written to %s (%zu bytes)\n", out_path.c_str(),
                results.size() * sizeof(ResultRecord));
    return 0;
}
