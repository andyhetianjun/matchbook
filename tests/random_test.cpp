// Randomized differential test: the same operations go to the real book and to
// the slow reference book, and every answer has to match.

#include <gtest/gtest.h>

#include <random>
#include <sstream>

#include "matchbook/order_book.hpp"
#include "reference_book.hpp"

using namespace matchbook;

namespace {

struct Params {
    std::uint64_t seed;
    int ops;
    Price low;
    Price high;
    OrderId id_space;  // small id spaces force duplicates and unknown-id cancels
};

std::string describe(const Order& o) {
    std::ostringstream s;
    s << (o.side == Side::Buy ? "BUY" : "SELL") << (o.type == OrderType::Market ? " MKT" : " LMT")
      << " tif=" << static_cast<int>(o.tif) << " id=" << o.id << " px=" << o.price
      << " qty=" << o.qty;
    return s.str();
}

void run(const Params& p) {
    std::mt19937_64 rng(p.seed);
    auto pick = [&](std::uint64_t n) { return rng() % n; };

    OrderBook book;
    reference::ReferenceBook ref;
    std::vector<Trade> got, want;

    for (int step = 0; step < p.ops; ++step) {
        got.clear();
        want.clear();
        SubmitResult a, b;
        std::string what;
        const auto kind = pick(100);
        const OrderId id = 1 + pick(p.id_space);

        if (kind < 70) {
            Order o;
            o.id = id;
            o.side = pick(2) ? Side::Buy : Side::Sell;
            o.qty = static_cast<Qty>(1 + pick(20));
            if (pick(100) < 8) {
                o.type = OrderType::Market;
            } else {
                o.type = OrderType::Limit;
                o.price = p.low +
                          static_cast<Price>(pick(static_cast<std::uint64_t>(p.high - p.low + 1)));
            }
            const auto t = pick(100);
            o.tif = t < 70 ? TimeInForce::GTC : (t < 88 ? TimeInForce::IOC : TimeInForce::FOK);
            if (pick(200) == 0) o.qty = 0;  // occasional invalid order
            what = "submit " + describe(o);
            a = book.submit(o, got);
            b = ref.submit(o, want);
        } else if (kind < 90) {
            what = "cancel id=" + std::to_string(id);
            const bool x = book.cancel(id);
            const bool y = ref.cancel(id);
            ASSERT_EQ(x, y) << "seed " << p.seed << " step " << step << ": " << what;
            a = b = SubmitResult{};
        } else {
            const Price price =
                p.low + static_cast<Price>(pick(static_cast<std::uint64_t>(p.high - p.low + 1)));
            const Qty qty = static_cast<Qty>(1 + pick(20));
            what = "modify id=" + std::to_string(id) + " px=" + std::to_string(price) +
                   " qty=" + std::to_string(qty);
            a = book.modify(id, price, qty, got);
            b = ref.modify(id, price, qty, want);
        }

        const std::string where =
            "seed " + std::to_string(p.seed) + " step " + std::to_string(step) + ": " + what;
        ASSERT_EQ(a, b) << where;
        ASSERT_EQ(got, want) << where;
        ASSERT_EQ(book.validate(), "") << where;
        ASSERT_EQ(book.best_bid(), ref.best_bid()) << where;
        ASSERT_EQ(book.best_ask(), ref.best_ask()) << where;
        ASSERT_EQ(book.order_count(), ref.order_count()) << where;
        ASSERT_EQ(book.depth(Side::Buy, 1000), ref.depth(Side::Buy, 1000)) << where;
        ASSERT_EQ(book.depth(Side::Sell, 1000), ref.depth(Side::Sell, 1000)) << where;
        if (step % 50 == 0) {
            for (OrderId probe = 1; probe <= p.id_space; ++probe) {
                ASSERT_EQ(book.find(probe), ref.find(probe)) << where << " probe " << probe;
            }
        }
    }
}

}  // namespace

TEST(RandomAgainstReference, TightPricesManyCrossings) {
    for (std::uint64_t seed = 1; seed <= 25; ++seed) run({seed, 4000, 98, 102, 300});
}

TEST(RandomAgainstReference, WidePrices) {
    for (std::uint64_t seed = 100; seed < 110; ++seed) run({seed, 4000, 50, 150, 2000});
}

TEST(RandomAgainstReference, TinyIdSpaceForcesDuplicatesAndStaleCancels) {
    for (std::uint64_t seed = 200; seed < 210; ++seed) run({seed, 3000, 95, 105, 15});
}

TEST(RandomAgainstReference, LongRun) {
    run({999, 40000, 90, 110, 1500});
}
