// Throughput of the whole exchange (several books, the feed and the ledger), using the
// same seeded traffic as the browser demo. Compare with replay_bench, which times one
// bare order book. Usage: matchbook_market_bench [--ops N] [--symbols N] [--seed N] [--runs N]

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "matchbook/traffic.hpp"

using namespace matchbook;

int main(int argc, char** argv) {
    std::size_t ops = 2000000, symbols = 3, runs = 5;
    std::uint64_t seed = 42;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        const auto v = std::strtoull(argv[i + 1], nullptr, 10);
        if (a == "--ops")
            ops = v;
        else if (a == "--symbols")
            symbols = v;
        else if (a == "--seed")
            seed = v;
        else if (a == "--runs")
            runs = v;
        else {
            std::cerr << "unknown option " << a << "\n";
            return 2;
        }
    }

    std::vector<double> rates;
    for (std::size_t r = 0; r < runs; ++r) {
        Exchange ex(1000);  // keep only the last 1000 ledger entries, as the demo does
        for (std::size_t s = 0; s < symbols; ++s) ex.add_symbol("S" + std::to_string(s));
        std::uint64_t messages = 0;
        ex.subscribe([&](const FeedMessage&) { ++messages; });
        TrafficGen gen(ex, {seed, 0, 8, 100});
        const auto t0 = std::chrono::steady_clock::now();
        const auto trades = gen.run(ops);
        const double secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        rates.push_back(static_cast<double>(ops) / secs);
        const std::string problem = ex.ledger().check();
        std::cout << "run " << r + 1 << ": " << static_cast<std::uint64_t>(rates.back())
                  << " ops/sec, " << trades << " trades, " << messages << " feed messages, ledger "
                  << (problem.empty() ? "balanced" : "NOT BALANCED: " + problem) << "\n";
        if (!problem.empty()) return 1;
    }
    std::sort(rates.begin(), rates.end());
    std::cout << "median " << static_cast<std::uint64_t>(rates[rates.size() / 2]) << " ops/sec ("
              << symbols << " symbols, " << ops << " ops, seed " << seed << ")\n";
    return 0;
}
