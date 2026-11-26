#include "matchbook/order_book.hpp"

#include <gtest/gtest.h>

using namespace matchbook;

namespace {

class BookTest : public ::testing::Test {
protected:
    SubmitResult submit(const Order& o) {
        trades.clear();
        auto r = book.submit(o, trades);
        EXPECT_EQ(book.validate(), "");
        return r;
    }
    SubmitResult buy(OrderId id, Price p, Qty q, TimeInForce t = TimeInForce::GTC) {
        return submit(limit_order(id, Side::Buy, p, q, t));
    }
    SubmitResult sell(OrderId id, Price p, Qty q, TimeInForce t = TimeInForce::GTC) {
        return submit(limit_order(id, Side::Sell, p, q, t));
    }
    bool cancel(OrderId id) {
        bool r = book.cancel(id);
        EXPECT_EQ(book.validate(), "");
        return r;
    }
    SubmitResult modify(OrderId id, Price p, Qty q) {
        trades.clear();
        auto r = book.modify(id, p, q, trades);
        EXPECT_EQ(book.validate(), "");
        return r;
    }

    OrderBook book;
    std::vector<Trade> trades;
};

}  // namespace

TEST_F(BookTest, EmptyBook) {
    EXPECT_FALSE(book.best_bid());
    EXPECT_FALSE(book.best_ask());
    EXPECT_TRUE(book.depth(Side::Buy, 5).empty());
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_FALSE(book.cancel(1));
}

TEST_F(BookTest, RestingOrdersSetBestPrices) {
    auto r = buy(1, 100, 10);
    EXPECT_EQ(r, (SubmitResult{Status::Resting, RejectReason::None, 0, 10}));
    sell(2, 105, 7);
    buy(3, 101, 1);
    EXPECT_EQ(book.best_bid(), 101);
    EXPECT_EQ(book.best_ask(), 105);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.order_count(), 3u);
}

TEST_F(BookTest, DepthAggregatesAndOrders) {
    buy(1, 100, 5);
    buy(2, 100, 3);
    buy(3, 99, 4);
    buy(4, 98, 1);
    sell(5, 101, 2);
    sell(6, 103, 6);
    const std::vector<DepthLevel> bids{{100, 8, 2}, {99, 4, 1}};
    EXPECT_EQ(book.depth(Side::Buy, 2), bids);
    EXPECT_EQ(book.depth(Side::Buy, 10).size(), 3u);
    const std::vector<DepthLevel> asks{{101, 2, 1}, {103, 6, 1}};
    EXPECT_EQ(book.depth(Side::Sell, 10), asks);
    EXPECT_TRUE(book.depth(Side::Sell, 0).empty());
}

TEST_F(BookTest, NonCrossingPricesDoNotTrade) {
    sell(1, 101, 5);
    auto r = buy(2, 100, 5);
    EXPECT_EQ(r.status, Status::Resting);
    EXPECT_TRUE(trades.empty());
}

TEST_F(BookTest, EqualPricesTrade) {
    sell(1, 100, 5);
    auto r = buy(2, 100, 5);
    EXPECT_EQ(r, (SubmitResult{Status::Filled, RejectReason::None, 5, 0}));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{2, 1, Side::Buy, 100, 5}));
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_FALSE(book.best_ask());
}

TEST_F(BookTest, TradesAtMakerPrice) {
    sell(1, 100, 5);
    buy(2, 105, 5);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 100);

    buy(3, 90, 5);
    sell(4, 80, 5);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{4, 3, Side::Sell, 90, 5}));
}

TEST_F(BookTest, PricePriority) {
    sell(1, 102, 5);
    sell(2, 100, 5);
    sell(3, 101, 5);
    buy(4, 102, 10);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].maker_id, 2u);
    EXPECT_EQ(trades[1].maker_id, 3u);
    EXPECT_EQ(book.best_ask(), 102);
}

TEST_F(BookTest, TimePriorityWithinLevel) {
    buy(1, 100, 3);
    buy(2, 100, 3);
    buy(3, 100, 3);
    sell(4, 100, 7);
    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0], (Trade{4, 1, Side::Sell, 100, 3}));
    EXPECT_EQ(trades[1], (Trade{4, 2, Side::Sell, 100, 3}));
    EXPECT_EQ(trades[2], (Trade{4, 3, Side::Sell, 100, 1}));
    EXPECT_EQ(book.find(3)->qty, 2);
}

TEST_F(BookTest, PartiallyFilledMakerKeepsItsPlace) {
    sell(1, 100, 10);
    sell(2, 100, 10);
    buy(3, 100, 4);  // takes 4 of order 1
    buy(4, 100, 8);  // takes the last 6 of order 1, then 2 of order 2
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0], (Trade{4, 1, Side::Buy, 100, 6}));
    EXPECT_EQ(trades[1], (Trade{4, 2, Side::Buy, 100, 2}));
    EXPECT_FALSE(book.find(1));
    EXPECT_EQ(book.find(2)->qty, 8);
}

TEST_F(BookTest, PartiallyFilledTakerRestsRemainder) {
    sell(1, 100, 4);
    auto r = buy(2, 101, 10);
    EXPECT_EQ(r, (SubmitResult{Status::Resting, RejectReason::None, 4, 6}));
    EXPECT_EQ(book.best_bid(), 101);
    EXPECT_FALSE(book.best_ask());
    EXPECT_EQ(book.find(2)->qty, 6);
}

TEST_F(BookTest, TakerSweepsSeveralLevels) {
    sell(1, 100, 2);
    sell(2, 101, 2);
    sell(3, 102, 2);
    sell(4, 103, 2);
    auto r = buy(5, 102, 7);
    EXPECT_EQ(r, (SubmitResult{Status::Resting, RejectReason::None, 6, 1}));
    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[2].price, 102);
    EXPECT_EQ(book.best_bid(), 102);
    EXPECT_EQ(book.best_ask(), 103);
}

TEST_F(BookTest, MarketOrderSweepsAndNeverRests) {
    sell(1, 100, 3);
    sell(2, 105, 3);
    auto r = submit(market_order(3, Side::Buy, 10));
    EXPECT_EQ(r, (SubmitResult{Status::Cancelled, RejectReason::None, 6, 4}));
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[1].price, 105);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_FALSE(book.best_bid());
}

TEST_F(BookTest, MarketOrderOnEmptyBook) {
    auto r = submit(market_order(1, Side::Sell, 5));
    EXPECT_EQ(r, (SubmitResult{Status::Cancelled, RejectReason::None, 0, 5}));
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(BookTest, MarketOrderWithGtcStillDoesNotRest) {
    auto r = submit(market_order(1, Side::Buy, 5, TimeInForce::GTC));
    EXPECT_EQ(r.status, Status::Cancelled);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(BookTest, MarketOrderFullyFilled) {
    buy(1, 100, 5);
    buy(2, 99, 5);
    auto r = submit(market_order(3, Side::Sell, 8));
    EXPECT_EQ(r, (SubmitResult{Status::Filled, RejectReason::None, 8, 0}));
    EXPECT_EQ(book.find(2)->qty, 2);
}

TEST_F(BookTest, IocFillsWhatItCanAndDropsTheRest) {
    sell(1, 100, 3);
    sell(2, 102, 3);
    auto r = buy(3, 101, 5, TimeInForce::IOC);
    EXPECT_EQ(r, (SubmitResult{Status::Cancelled, RejectReason::None, 3, 2}));
    EXPECT_FALSE(book.find(3));
    EXPECT_EQ(book.best_ask(), 102);
    EXPECT_FALSE(book.best_bid());
}

TEST_F(BookTest, IocWithNothingToTrade) {
    sell(1, 105, 3);
    auto r = buy(2, 100, 5, TimeInForce::IOC);
    EXPECT_EQ(r, (SubmitResult{Status::Cancelled, RejectReason::None, 0, 5}));
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.order_count(), 1u);
}

TEST_F(BookTest, IocFullyFilled) {
    sell(1, 100, 5);
    auto r = buy(2, 100, 5, TimeInForce::IOC);
    EXPECT_EQ(r.status, Status::Filled);
}

TEST_F(BookTest, FokFillsWhenEnoughLiquidityAcrossLevels) {
    sell(1, 100, 3);
    sell(2, 101, 3);
    sell(3, 102, 3);
    auto r = buy(4, 102, 8, TimeInForce::FOK);
    EXPECT_EQ(r, (SubmitResult{Status::Filled, RejectReason::None, 8, 0}));
    EXPECT_EQ(trades.size(), 3u);
    EXPECT_EQ(book.find(3)->qty, 1);
}

TEST_F(BookTest, FokExactlyEnough) {
    sell(1, 100, 5);
    auto r = buy(2, 100, 5, TimeInForce::FOK);
    EXPECT_EQ(r.status, Status::Filled);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(BookTest, FokKilledWithoutTouchingTheBook) {
    sell(1, 100, 3);
    sell(2, 101, 3);
    const auto before = book.depth(Side::Sell, 10);
    auto r = buy(3, 101, 7, TimeInForce::FOK);
    EXPECT_EQ(r, (SubmitResult{Status::Cancelled, RejectReason::None, 0, 7}));
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.depth(Side::Sell, 10), before);
    EXPECT_FALSE(book.find(3));
}

TEST_F(BookTest, FokIgnoresLiquidityBeyondItsLimit) {
    sell(1, 100, 3);
    sell(2, 105, 10);
    auto r = buy(3, 101, 5, TimeInForce::FOK);
    EXPECT_EQ(r.status, Status::Cancelled);
    EXPECT_TRUE(trades.empty());
}

TEST_F(BookTest, MarketFok) {
    buy(1, 100, 3);
    buy(2, 99, 3);
    auto killed = submit(market_order(3, Side::Sell, 7, TimeInForce::FOK));
    EXPECT_EQ(killed.status, Status::Cancelled);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.order_count(), 2u);
    auto ok = submit(market_order(4, Side::Sell, 6, TimeInForce::FOK));
    EXPECT_EQ(ok.status, Status::Filled);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(BookTest, CancelRemovesOrderAndEmptyLevel) {
    buy(1, 100, 5);
    buy(2, 99, 5);
    EXPECT_TRUE(cancel(1));
    EXPECT_EQ(book.best_bid(), 99);
    EXPECT_FALSE(book.find(1));
    EXPECT_FALSE(cancel(1));
}

TEST_F(BookTest, CancelFromMiddleOfQueueKeepsOthersInOrder) {
    for (OrderId id = 1; id <= 5; ++id) sell(id, 100, 2);
    EXPECT_TRUE(cancel(3));
    EXPECT_EQ(book.depth(Side::Sell, 1)[0], (DepthLevel{100, 8, 4}));
    buy(10, 100, 8);
    ASSERT_EQ(trades.size(), 4u);
    EXPECT_EQ(trades[0].maker_id, 1u);
    EXPECT_EQ(trades[1].maker_id, 2u);
    EXPECT_EQ(trades[2].maker_id, 4u);
    EXPECT_EQ(trades[3].maker_id, 5u);
}

TEST_F(BookTest, CancelHeadAndTail) {
    for (OrderId id = 1; id <= 3; ++id) buy(id, 100, 1);
    EXPECT_TRUE(cancel(1));
    EXPECT_TRUE(cancel(3));
    sell(4, 100, 5);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_id, 2u);
}

TEST_F(BookTest, CannotCancelFilledOrder) {
    sell(1, 100, 5);
    buy(2, 100, 5);
    EXPECT_FALSE(cancel(1));
    EXPECT_FALSE(cancel(2));
}

TEST_F(BookTest, RejectsBadInput) {
    EXPECT_EQ(buy(1, 100, 0).reason, RejectReason::BadQuantity);
    EXPECT_EQ(buy(1, 100, -5).reason, RejectReason::BadQuantity);
    EXPECT_EQ(buy(1, 0, 5).reason, RejectReason::BadPrice);
    EXPECT_EQ(buy(1, -3, 5).reason, RejectReason::BadPrice);
    EXPECT_EQ(submit(market_order(1, Side::Buy, 0)).reason, RejectReason::BadQuantity);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(BookTest, RejectsDuplicateLiveId) {
    buy(1, 100, 5);
    auto r = sell(1, 110, 5);
    EXPECT_EQ(r.status, Status::Rejected);
    EXPECT_EQ(r.reason, RejectReason::DuplicateId);
    EXPECT_FALSE(book.best_ask());
    EXPECT_EQ(book.find(1)->side, Side::Buy);
}

TEST_F(BookTest, IdCanBeReusedOnceItIsGone) {
    buy(1, 100, 5);
    cancel(1);
    EXPECT_EQ(buy(1, 99, 2).status, Status::Resting);
    sell(2, 99, 2);
    EXPECT_EQ(buy(1, 98, 1).status, Status::Resting);
}

TEST_F(BookTest, ModifyDownKeepsQueuePosition) {
    buy(1, 100, 10);
    buy(2, 100, 10);
    auto r = modify(1, 100, 4);
    EXPECT_EQ(r, (SubmitResult{Status::Resting, RejectReason::None, 0, 4}));
    EXPECT_EQ(book.depth(Side::Buy, 1)[0], (DepthLevel{100, 14, 2}));
    sell(3, 100, 5);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0], (Trade{3, 1, Side::Sell, 100, 4}));
    EXPECT_EQ(trades[1], (Trade{3, 2, Side::Sell, 100, 1}));
}

TEST_F(BookTest, ModifyUpLosesQueuePosition) {
    buy(1, 100, 5);
    buy(2, 100, 5);
    modify(1, 100, 8);
    sell(3, 100, 5);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_id, 2u);
    EXPECT_EQ(book.find(1)->qty, 8);
}

TEST_F(BookTest, ModifyPriceMovesOrderToNewLevelAtBack) {
    buy(1, 100, 5);
    buy(2, 101, 5);
    buy(3, 101, 5);
    modify(1, 101, 5);
    EXPECT_EQ(book.depth(Side::Buy, 5), (std::vector<DepthLevel>{{101, 15, 3}}));
    sell(4, 101, 11);
    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[2].maker_id, 1u);
}

TEST_F(BookTest, ModifyToCrossingPriceTrades) {
    sell(1, 105, 5);
    buy(2, 100, 8);
    auto r = modify(2, 105, 8);
    EXPECT_EQ(r, (SubmitResult{Status::Resting, RejectReason::None, 5, 3}));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{2, 1, Side::Buy, 105, 5}));
    EXPECT_EQ(book.best_bid(), 105);
}

TEST_F(BookTest, ModifySameValuesChangesNothing) {
    buy(1, 100, 5);
    buy(2, 100, 5);
    modify(1, 100, 5);
    sell(3, 100, 1);
    EXPECT_EQ(trades[0].maker_id, 1u);
}

TEST_F(BookTest, ModifyRejections) {
    buy(1, 100, 5);
    EXPECT_EQ(modify(9, 100, 5).reason, RejectReason::UnknownId);
    EXPECT_EQ(modify(1, 100, 0).reason, RejectReason::BadQuantity);
    EXPECT_EQ(modify(1, 0, 5).reason, RejectReason::BadPrice);
    EXPECT_EQ(book.find(1)->qty, 5);
}

TEST_F(BookTest, LargeQuantitiesDoNotOverflowTotals) {
    const Qty big = Qty{1} << 40;
    sell(1, 100, big);
    sell(2, 100, big);
    buy(3, 100, big + 1);
    EXPECT_EQ(book.depth(Side::Sell, 1)[0].qty, big - 1);
}

TEST_F(BookTest, BookStaysUncrossedThroughASequence) {
    for (OrderId id = 1; id <= 20; ++id) {
        buy(id, 90 + static_cast<Price>(id % 7), 3);
        sell(100 + id, 94 + static_cast<Price>(id % 5), 2);
        if (book.best_bid() && book.best_ask()) {
            EXPECT_LT(*book.best_bid(), *book.best_ask());
        }
    }
}

TEST_F(BookTest, PoolSlotsAreReusedAfterCancelsAndFills) {
    // Churn: many rounds of add, partial fill, cancel. validate() checks that no
    // pool slot is leaked or freed twice.
    OrderId next = 1;
    for (int round = 0; round < 200; ++round) {
        const OrderId first = next;
        for (int i = 0; i < 20; ++i) buy(next++, 100 - (i % 3), 5);
        sell(next++, 99, 12);
        for (OrderId id = first; id < first + 20; id += 2) cancel(id);
        submit(market_order(next++, Side::Sell, 1000));
        EXPECT_EQ(book.order_count(), 0u);
    }
}
