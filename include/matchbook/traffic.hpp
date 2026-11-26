#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "matchbook/exchange.hpp"

namespace matchbook {

// A seeded stream of random orders for every symbol of an exchange. The same seed
// always gives the same stream. The mix follows bench/replay_bench.cpp: mostly
// passive limit orders near the middle price, some that cross, a few IOC, FOK and
// market orders, and cancels and modifies of recent orders.
class TrafficGen {
public:
    struct Config {
        std::uint64_t seed = 1;
        Account first_account = 0;
        Account accounts = 8;   // orders come from first_account .. first_account + accounts - 1
        Price start_mid = 100;  // each symbol starts around this price (in ticks)
    };

    TrafficGen(Exchange& ex, const Config& cfg);

    // Runs `n` operations (new orders, cancels, modifies). Returns the number of trades.
    std::uint64_t run(std::size_t n);

private:
    struct Recent {
        OrderId id;
        Account account;
    };
    Exchange& ex_;
    Config cfg_;
    std::mt19937_64 rng_;
    std::vector<Price> mid_;
    std::vector<Recent> recent_;
    std::size_t recent_next_ = 0;
};

}  // namespace matchbook
