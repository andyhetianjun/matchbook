// Randomized tests for the multi-symbol exchange. Each stream is checked for:
//  - every book passes validate() and the feed has no gaps,
//  - a client that rebuilds the book from the feed (from the start, and one that
//    joins in the middle from a snapshot) always ends up equal to the real book,
//  - the ledger balances, and matches balances computed here from the trade prints
//    and our own record of who placed which order.

#include <gtest/gtest.h>

#include <map>
#include <random>

#include "matchbook/exchange.hpp"
#include "matchbook/traffic.hpp"

using namespace matchbook;

namespace {

std::vector<DepthLevel> strip(std::vector<DepthLevel> v) {
    for (auto& l : v) l.orders = 0;
    return v;
}

void expect_mirror_matches(const Exchange& ex, SymbolId s, const BookMirror& m) {
    const auto snap = ex.snapshot(s);
    EXPECT_EQ(m.seq(), snap.seq);
    EXPECT_EQ(m.bids(), strip(snap.bids));
    EXPECT_EQ(m.asks(), strip(snap.asks));
    if (auto top = m.top()) {
        const auto& b = snap.bids;
        const auto& a = snap.asks;
        EXPECT_EQ(top->bid_qty, b.empty() ? 0 : b[0].qty);
        EXPECT_EQ(top->ask_qty, a.empty() ? 0 : a[0].qty);
        if (!b.empty()) EXPECT_EQ(top->bid_price, b[0].price);
        if (!a.empty()) EXPECT_EQ(top->ask_price, a[0].price);
    }
}

struct Params {
    std::uint64_t seed;
    int ops;
    int symbols;
    Account accounts;
    Price low, high;
};

void run(const Params& p) {
    SCOPED_TRACE("seed " + std::to_string(p.seed));
    std::mt19937_64 rng(p.seed);
    auto pick = [&](std::uint64_t n) { return rng() % n; };

    Exchange ex;
    for (int i = 0; i < p.symbols; ++i) ex.add_symbol("S" + std::to_string(i));

    std::vector<BookMirror> from_start(p.symbols);
    std::vector<BookMirror> late(p.symbols);
    std::vector<bool> joined(p.symbols, false);
    std::map<OrderId, Account> placed_by;  // our own record
    std::map<Account, std::int64_t> cash;  // expected, from trade prints
    std::map<std::pair<Account, SymbolId>, Qty> pos;
    std::uint64_t prints = 0;
    std::vector<std::uint64_t> last_seq(p.symbols, 0);
    std::vector<OrderId> ids;

    ex.subscribe([&](const FeedMessage& m) {
        EXPECT_EQ(m.seq, last_seq[m.symbol] + 1) << "gap in the feed";
        last_seq[m.symbol] = m.seq;
        EXPECT_EQ(from_start[m.symbol].apply(m), BookMirror::Apply::Applied);
        if (joined[m.symbol]) EXPECT_EQ(late[m.symbol].apply(m), BookMirror::Apply::Applied);
        if (const auto* t = std::get_if<TradePrint>(&m.body)) {
            ++prints;
            const Account taker = placed_by.at(t->taker), maker = placed_by.at(t->maker);
            const Account buyer = t->taker_side == Side::Buy ? taker : maker;
            const Account seller = t->taker_side == Side::Buy ? maker : taker;
            cash[buyer] -= t->price * t->qty;
            cash[seller] += t->price * t->qty;
            pos[{buyer, m.symbol}] += t->qty;
            pos[{seller, m.symbol}] -= t->qty;
        }
    });

    for (int step = 0; step < p.ops; ++step) {
        if (step == p.ops / 2) {  // some listeners join half way through
            for (int s = 0; s < p.symbols; ++s) {
                late[s].load(ex.snapshot(static_cast<SymbolId>(s)));
                joined[s] = true;
            }
        }
        const auto kind = pick(100);
        const Account acct = static_cast<Account>(pick(p.accounts));
        if (kind < 78 || ids.empty()) {
            NewOrder o;
            o.account = acct;
            o.symbol = static_cast<SymbolId>(pick(p.symbols));
            o.side = pick(2) ? Side::Buy : Side::Sell;
            o.qty = static_cast<Qty>(1 + pick(20));
            if (pick(100) < 8) {
                o.type = OrderType::Market;
            } else {
                o.price = p.low +
                          static_cast<Price>(pick(static_cast<std::uint64_t>(p.high - p.low + 1)));
            }
            const auto t = pick(100);
            o.tif = t < 15 ? TimeInForce::IOC : t < 22 ? TimeInForce::FOK : TimeInForce::GTC;
            // We need to know the id before the trades are published, and ids are
            // handed out in order, so predict it.
            const OrderId next = ids.empty() ? 1 : ids.back() + 1;
            placed_by[next] = acct;
            const Placed r = ex.submit(o);
            ASSERT_EQ(r.id, next);
            ids.push_back(r.id);
        } else if (kind < 92) {
            const OrderId id = ids[pick(ids.size())];
            // half the time use the wrong account, which must never work
            const bool right = pick(2);
            const Account who = right ? placed_by[id] : (placed_by[id] + 1) % p.accounts;
            const bool ok = ex.cancel(who, id);
            if (!right && p.accounts > 1) EXPECT_FALSE(ok);
        } else {
            const OrderId id = ids[pick(ids.size())];
            ex.modify(
                placed_by[id], id,
                p.low + static_cast<Price>(pick(static_cast<std::uint64_t>(p.high - p.low + 1))),
                static_cast<Qty>(1 + pick(20)));
        }
        if (step % 97 == 0) {
            for (int s = 0; s < p.symbols; ++s) {
                ASSERT_EQ(ex.book(static_cast<SymbolId>(s)).validate(), "");
                expect_mirror_matches(ex, static_cast<SymbolId>(s), from_start[s]);
                if (joined[s]) expect_mirror_matches(ex, static_cast<SymbolId>(s), late[s]);
            }
            ASSERT_EQ(ex.ledger().check(), "") << "at step " << step;
        }
        if (::testing::Test::HasFailure()) return;
    }

    for (int s = 0; s < p.symbols; ++s) {
        ASSERT_EQ(ex.book(static_cast<SymbolId>(s)).validate(), "");
        expect_mirror_matches(ex, static_cast<SymbolId>(s), from_start[s]);
        expect_mirror_matches(ex, static_cast<SymbolId>(s), late[s]);
    }
    const Ledger& l = ex.ledger();
    EXPECT_EQ(l.check(), "");
    EXPECT_EQ(l.trade_count(), prints);

    // The ledger must agree with balances worked out independently from the feed.
    std::int64_t total_cash = 0;
    for (Account a = 0; a < p.accounts; ++a) {
        EXPECT_EQ(l.cash(a), cash[a]) << "account " << a;
        total_cash += l.cash(a);
        for (int s = 0; s < p.symbols; ++s) {
            EXPECT_EQ(l.position(a, static_cast<SymbolId>(s)),
                      (pos[{a, static_cast<SymbolId>(s)}]));
        }
    }
    EXPECT_EQ(total_cash, 0);
    for (int s = 0; s < p.symbols; ++s) {
        Qty sum = 0;
        for (Account a = 0; a < p.accounts; ++a) sum += l.position(a, static_cast<SymbolId>(s));
        EXPECT_EQ(sum, 0) << "symbol " << s;
    }
}

}  // namespace

TEST(MarketRandom, ManySeeds) {
    for (std::uint64_t seed = 1; seed <= 24; ++seed) {
        run({seed, 4000, 1 + static_cast<int>(seed % 4), static_cast<Account>(2 + seed % 6), 95,
             105});
    }
}

TEST(MarketRandom, OneAccountTradesWithItself) {
    run({77, 4000, 2, 1, 98, 102});
}

TEST(MarketRandom, LongStreamWideRange) {
    run({999, 40000, 3, 10, 50, 150});
}

TEST(Traffic, SameSeedGivesTheSameMarket) {
    auto go = [](std::uint64_t seed) {
        Exchange ex;
        ex.add_symbol("A");
        ex.add_symbol("B");
        TrafficGen g(ex, {seed, 6, 100});
        const auto trades = g.run(20000);
        return std::make_tuple(trades, ex.ledger().traded_cash(), ex.seq(0), ex.seq(1),
                               ex.book(0).order_count(), ex.book(1).order_count());
    };
    EXPECT_EQ(go(5), go(5));
    EXPECT_NE(go(5), go(6));
}

TEST(Traffic, LedgerBalancesAndBooksAreValid) {
    Exchange ex(1000);
    for (const char* s : {"A", "B", "C"}) ex.add_symbol(s);
    TrafficGen g(ex, {42, 8, 100});
    std::uint64_t trades = 0;
    for (int i = 0; i < 20; ++i) {
        trades += g.run(10000);
        ASSERT_EQ(ex.ledger().check(), "");
    }
    EXPECT_GT(trades, 10000u);
    EXPECT_EQ(trades, ex.ledger().trade_count());
    for (SymbolId s = 0; s < 3; ++s) EXPECT_EQ(ex.book(s).validate(), "");
}
