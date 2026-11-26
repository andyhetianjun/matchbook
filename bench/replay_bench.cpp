// Replays a seeded, generated order stream through the book and reports
// throughput and per-operation latency.
//
//   matchbook_bench [--ops N] [--seed S] [--runs R] [--reserve N]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "matchbook/order_book.hpp"

using namespace matchbook;
using Clock = std::chrono::steady_clock;

namespace {

struct Op {
    enum class Kind : std::uint8_t { New, Cancel, Modify } kind;
    Order order;  // New
    OrderId id;   // Cancel, Modify
    Price price;  // Modify
    Qty qty;      // Modify
};

struct Options {
    std::size_t ops = 2'000'000;
    std::uint64_t seed = 42;
    int runs = 5;
    std::size_t reserve = 0;
};

// Mix: 62% passive limit, 13% aggressive limit, 8% IOC, 2% market, 1% FOK
// (new orders), 10% cancel, 4% modify. The mid price drifts slowly.
std::vector<Op> generate(const Options& opt) {
    std::mt19937_64 rng(opt.seed);
    auto pick = [&](std::uint64_t n) { return rng() % n; };
    std::vector<Op> ops;
    ops.reserve(opt.ops);
    Price mid = 10'000;
    OrderId next_id = 1;
    for (std::size_t i = 0; i < opt.ops; ++i) {
        if (i % 200 == 0) mid += static_cast<Price>(pick(3)) - 1;
        const auto roll = pick(100);
        Op op{};
        if (roll < 86) {
            Order o;
            o.id = next_id++;
            o.side = pick(2) ? Side::Buy : Side::Sell;
            o.qty = static_cast<Qty>(1 + pick(100));
            const Price away = static_cast<Price>(pick(40));  // distance from mid in ticks
            const Price sign = o.side == Side::Buy ? -1 : 1;
            if (roll < 62) {
                o.price = mid + sign * (1 + away);  // passive, rests
            } else if (roll < 75) {
                o.price = mid - sign * static_cast<Price>(pick(6));  // crosses the mid
            } else if (roll < 83) {
                o.price = mid - sign * static_cast<Price>(pick(4));
                o.tif = TimeInForce::IOC;
            } else if (roll < 85) {
                o.type = OrderType::Market;
                o.tif = TimeInForce::IOC;
                o.qty = static_cast<Qty>(1 + pick(30));
            } else {
                o.price = mid - sign * static_cast<Price>(pick(3));
                o.tif = TimeInForce::FOK;
                o.qty = static_cast<Qty>(1 + pick(30));
            }
            op.kind = Op::Kind::New;
            op.order = o;
        } else {
            // Target one of the most recent 5000 ids, which are likely still resting.
            const OrderId lo = next_id > 5000 ? next_id - 5000 : 1;
            op.id = lo + pick(next_id - lo);
            if (roll < 96) {
                op.kind = Op::Kind::Cancel;
            } else {
                op.kind = Op::Kind::Modify;
                op.price = mid + static_cast<Price>(pick(41)) - 20;
                op.qty = static_cast<Qty>(1 + pick(100));
            }
        }
        ops.push_back(op);
    }
    return ops;
}

struct Counters {
    std::uint64_t trades = 0;
    std::uint64_t traded_qty = 0;
    std::uint64_t cancels_hit = 0;
};

inline void apply(OrderBook& book, const Op& op, std::vector<Trade>& trades, Counters& c) {
    trades.clear();
    switch (op.kind) {
        case Op::Kind::New:
            book.submit(op.order, trades);
            break;
        case Op::Kind::Cancel:
            c.cancels_hit += book.cancel(op.id) ? 1 : 0;
            break;
        case Op::Kind::Modify:
            book.modify(op.id, op.price, op.qty, trades);
            break;
    }
    c.trades += trades.size();
    for (const Trade& t : trades) c.traded_qty += static_cast<std::uint64_t>(t.qty);
}

double pct(const std::vector<std::uint32_t>& sorted, double p) {
    const std::size_t i = std::min(sorted.size() - 1, static_cast<std::size_t>(p * sorted.size()));
    return sorted[i];
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const std::string val = argv[i + 1];
        if (key == "--ops")
            opt.ops = std::stoull(val);
        else if (key == "--seed")
            opt.seed = std::stoull(val);
        else if (key == "--runs")
            opt.runs = std::stoi(val);
        else if (key == "--reserve")
            opt.reserve = std::stoull(val);
        else {
            std::cerr << "unknown option " << key << '\n';
            return 2;
        }
    }

    const auto ops = generate(opt);
    std::cout << "ops=" << opt.ops << " seed=" << opt.seed << " runs=" << opt.runs
              << " reserve=" << opt.reserve << '\n';

    std::vector<Trade> trades;
    trades.reserve(1024);

    // Throughput: time the whole replay, no clock calls inside the loop.
    std::vector<double> rates;
    Counters last;
    std::size_t resting = 0, bid_levels = 0, ask_levels = 0;
    for (int r = 0; r < opt.runs; ++r) {
        OrderBook book(opt.reserve);
        Counters c;
        const auto t0 = Clock::now();
        for (const Op& op : ops) apply(book, op, trades, c);
        const auto t1 = Clock::now();
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        rates.push_back(static_cast<double>(ops.size()) / secs);
        std::cout << "  run " << r + 1 << ": " << static_cast<long long>(rates.back())
                  << " ops/sec\n";
        last = c;
        resting = book.order_count();
        bid_levels = book.depth(Side::Buy, 1u << 30).size();
        ask_levels = book.depth(Side::Sell, 1u << 30).size();
    }
    std::sort(rates.begin(), rates.end());
    std::cout << "throughput (median of runs): " << static_cast<long long>(rates[rates.size() / 2])
              << " ops/sec\n";
    std::cout << "trades=" << last.trades << " traded_qty=" << last.traded_qty
              << " cancels_hit=" << last.cancels_hit << " resting_at_end=" << resting
              << " levels=" << bid_levels << "+" << ask_levels << '\n';

    // Latency: time every operation on its own. The two clock reads cost
    // something, so measure that too and print it.
    {
        std::vector<std::uint32_t> overhead;
        overhead.reserve(100000);
        for (int i = 0; i < 100000; ++i) {
            const auto a = Clock::now();
            const auto b = Clock::now();
            overhead.push_back(static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
        }
        std::sort(overhead.begin(), overhead.end());
        std::cout << "clock overhead: p50=" << pct(overhead, 0.5)
                  << "ns p99=" << pct(overhead, 0.99) << "ns\n";

        OrderBook book(opt.reserve);
        Counters c;
        std::vector<std::uint32_t> lat;
        lat.reserve(ops.size());
        for (const Op& op : ops) {
            const auto a = Clock::now();
            apply(book, op, trades, c);
            const auto b = Clock::now();
            lat.push_back(static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
        }
        std::sort(lat.begin(), lat.end());
        std::cout << "latency per op (ns, includes clock overhead): p50=" << pct(lat, 0.5)
                  << " p90=" << pct(lat, 0.9) << " p99=" << pct(lat, 0.99)
                  << " p99.9=" << pct(lat, 0.999) << " max=" << lat.back() << '\n';
    }
    return 0;
}
