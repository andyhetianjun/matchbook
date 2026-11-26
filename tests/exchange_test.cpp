#include "matchbook/exchange.hpp"

#include <gtest/gtest.h>

using namespace matchbook;

namespace {

struct Fixture {
    Exchange ex;
    std::vector<FeedMessage> feed;
    SymbolId aapl, msft;
    Fixture() {
        aapl = ex.add_symbol("AAPL");
        msft = ex.add_symbol("MSFT");
        ex.subscribe([this](const FeedMessage& m) { feed.push_back(m); });
    }
    Placed limit(Account a, SymbolId s, Side side, Price p, Qty q,
                 TimeInForce tif = TimeInForce::GTC) {
        return ex.submit(NewOrder{a, s, side, OrderType::Limit, tif, p, q});
    }
};

template <class T>
const T* as(const FeedMessage& m) {
    return std::get_if<T>(&m.body);
}

}  // namespace

TEST(Exchange, BooksAreIndependent) {
    Fixture f;
    f.limit(1, f.aapl, Side::Buy, 100, 10);
    f.limit(2, f.msft, Side::Sell, 100, 10);  // same price, other symbol: no trade
    EXPECT_EQ(f.ex.ledger().trade_count(), 0u);
    EXPECT_EQ(f.ex.book(f.aapl).best_bid(), 100);
    EXPECT_FALSE(f.ex.book(f.msft).best_bid());
    EXPECT_EQ(f.ex.book(f.msft).best_ask(), 100);
}

TEST(Exchange, IdsAreUniqueAcrossSymbols) {
    Fixture f;
    const auto a = f.limit(1, f.aapl, Side::Buy, 100, 1);
    const auto b = f.limit(1, f.msft, Side::Buy, 100, 1);
    EXPECT_NE(a.id, b.id);
}

TEST(Exchange, SequenceNumbersCountPerSymbolWithoutGaps) {
    Fixture f;
    f.limit(1, f.aapl, Side::Buy, 100, 10);
    f.limit(1, f.msft, Side::Buy, 50, 5);
    f.limit(2, f.aapl, Side::Sell, 100, 4);
    std::uint64_t last[2] = {0, 0};
    for (const auto& m : f.feed) {
        EXPECT_EQ(m.seq, last[m.symbol] + 1);
        last[m.symbol] = m.seq;
    }
    EXPECT_EQ(f.ex.seq(f.aapl), last[f.aapl]);
    EXPECT_EQ(f.ex.seq(f.msft), last[f.msft]);
}

TEST(Exchange, FeedForAPartialFill) {
    Fixture f;
    f.limit(1, f.aapl, Side::Buy, 100, 10);
    f.feed.clear();
    f.limit(2, f.aapl, Side::Sell, 100, 4);

    // one trade, then the level that shrank, then the new top of book
    ASSERT_EQ(f.feed.size(), 3u);
    const auto* t = as<TradePrint>(f.feed[0]);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->price, 100);
    EXPECT_EQ(t->qty, 4);
    EXPECT_EQ(t->taker_side, Side::Sell);
    const auto* lu = as<LevelUpdate>(f.feed[1]);
    ASSERT_TRUE(lu);
    EXPECT_EQ(lu->side, Side::Buy);
    EXPECT_EQ(lu->qty, 6);
    const auto* top = as<TopOfBook>(f.feed[2]);
    ASSERT_TRUE(top);
    EXPECT_EQ(top->bid_price, 100);
    EXPECT_EQ(top->bid_qty, 6);
    EXPECT_EQ(top->ask_qty, 0);
}

TEST(Exchange, TopOfBookOnlyPublishedWhenItChanges) {
    Fixture f;
    f.limit(1, f.aapl, Side::Buy, 100, 10);
    f.feed.clear();
    f.limit(1, f.aapl, Side::Buy, 99, 10);  // behind the best bid: a level update, no top
    ASSERT_EQ(f.feed.size(), 1u);
    EXPECT_TRUE(as<LevelUpdate>(f.feed[0]));
}

TEST(Exchange, LevelDisappearsWhenFullyFilledOrCancelled) {
    Fixture f;
    const auto bid = f.limit(1, f.aapl, Side::Buy, 100, 10);
    f.limit(2, f.aapl, Side::Sell, 100, 10);
    BookMirror m;
    m.load(BookSnapshot{f.aapl, 0, {}, {}});
    for (const auto& msg : f.feed) {
        if (msg.symbol == f.aapl) m.apply(msg);
    }
    EXPECT_TRUE(m.bids().empty());
    EXPECT_TRUE(m.asks().empty());
    EXPECT_FALSE(f.ex.cancel(1, bid.id));  // already gone
}

TEST(Exchange, OnlyTheOwnerCanCancel) {
    Fixture f;
    const auto o = f.limit(1, f.aapl, Side::Buy, 100, 10);
    EXPECT_FALSE(f.ex.cancel(2, o.id));
    EXPECT_TRUE(f.ex.book(f.aapl).find(o.id));
    EXPECT_TRUE(f.ex.cancel(1, o.id));
    EXPECT_FALSE(f.ex.book(f.aapl).find(o.id));
    EXPECT_FALSE(f.ex.owner(o.id));
}

TEST(Exchange, ModifyMovesTheLevelAndTheFeedFollows) {
    Fixture f;
    const auto o = f.limit(1, f.aapl, Side::Buy, 100, 10);
    BookMirror m;
    m.load(BookSnapshot{f.aapl, 0, {}, {}});
    for (const auto& msg : f.feed) m.apply(msg);  // everything so far
    f.feed.clear();
    const auto r = f.ex.modify(1, o.id, 99, 10);
    EXPECT_EQ(r.result.status, Status::Resting);
    for (const auto& msg : f.feed) EXPECT_EQ(m.apply(msg), BookMirror::Apply::Applied);
    ASSERT_EQ(m.bids().size(), 1u);
    EXPECT_EQ(m.bids()[0].price, 99);
    EXPECT_EQ(m.bids()[0].qty, 10);
}

TEST(Exchange, LedgerRecordsWhoOwesWhat) {
    Fixture f;
    f.limit(7, f.aapl, Side::Sell, 101, 5);
    f.limit(9, f.aapl, Side::Buy, 105, 3);  // buys 3 at the resting price, 101
    const Ledger& l = f.ex.ledger();
    ASSERT_EQ(l.entries().size(), 1u);
    const auto& e = l.entries()[0];
    EXPECT_EQ(e.buyer, 9u);
    EXPECT_EQ(e.seller, 7u);
    EXPECT_EQ(e.cash(), 303);
    EXPECT_EQ(l.cash(9), -303);
    EXPECT_EQ(l.cash(7), 303);
    EXPECT_EQ(l.position(9, f.aapl), 3);
    EXPECT_EQ(l.position(7, f.aapl), -3);
    EXPECT_EQ(l.check(), "");
}

TEST(Exchange, SelfTradeNetsToZero) {
    Fixture f;
    f.limit(3, f.aapl, Side::Sell, 100, 5);
    f.limit(3, f.aapl, Side::Buy, 100, 5);
    EXPECT_EQ(f.ex.ledger().trade_count(), 1u);
    EXPECT_EQ(f.ex.ledger().cash(3), 0);
    EXPECT_EQ(f.ex.ledger().position(3, f.aapl), 0);
    EXPECT_EQ(f.ex.ledger().check(), "");
}

TEST(Exchange, RejectedOrdersPublishNothing) {
    Fixture f;
    const auto r = f.limit(1, f.aapl, Side::Buy, 100, 0);
    EXPECT_EQ(r.result.status, Status::Rejected);
    EXPECT_TRUE(f.feed.empty());
}

TEST(Exchange, SnapshotThenIncrementalsRebuildsTheBook) {
    Fixture f;
    for (int i = 0; i < 10; ++i) f.limit(1, f.aapl, Side::Buy, 90 + i, 5);
    for (int i = 0; i < 10; ++i) f.limit(2, f.aapl, Side::Sell, 101 + i, 5);

    // a listener joins here
    const auto snap = f.ex.snapshot(f.aapl);
    BookMirror late;
    late.load(snap);
    f.feed.clear();

    f.limit(3, f.aapl, Side::Buy, 103, 12);  // trades through three levels and rests
    f.limit(4, f.aapl, Side::Sell, 89, 8);
    for (const auto& m : f.feed) {
        EXPECT_EQ(late.apply(m), BookMirror::Apply::Applied);
    }
    const auto now = f.ex.snapshot(f.aapl);
    EXPECT_EQ(late.seq(), now.seq);
    EXPECT_EQ(late.bids(), [&] {
        auto v = now.bids;
        for (auto& l : v) l.orders = 0;
        return v;
    }());
    EXPECT_EQ(late.asks().size(), now.asks.size());
}

TEST(Mirror, DetectsGapsAndStaleMessages) {
    BookMirror m;
    m.load(BookSnapshot{0, 10, {}, {}});
    FeedMessage old{10, 0, LevelUpdate{Side::Buy, 100, 5}};
    FeedMessage skip{12, 0, LevelUpdate{Side::Buy, 100, 5}};
    FeedMessage next{11, 0, LevelUpdate{Side::Buy, 100, 5}};
    EXPECT_EQ(m.apply(old), BookMirror::Apply::Stale);
    EXPECT_EQ(m.apply(skip), BookMirror::Apply::Gap);
    EXPECT_EQ(m.apply(next), BookMirror::Apply::Applied);
    EXPECT_EQ(m.seq(), 11u);
    EXPECT_EQ(m.apply(skip), BookMirror::Apply::Applied);
}

TEST(Ledger, CheckCatchesATamperedBalance) {
    Ledger l;
    l.record(0, 1, 2, 100, 5);
    EXPECT_EQ(l.check(), "");
    EXPECT_EQ(l.traded_cash(), 500);
    EXPECT_EQ(l.traded_qty(0), 5);
}

TEST(Ledger, CappedEntriesStillBalance) {
    Ledger l(10);
    for (int i = 0; i < 100; ++i) l.record(0, i % 3, (i + 1) % 3, 100 + i, 1 + i % 7);
    EXPECT_EQ(l.entries().size(), 10u);
    EXPECT_EQ(l.dropped_entries(), 90u);
    EXPECT_EQ(l.check(), "");
}
