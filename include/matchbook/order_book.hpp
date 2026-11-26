#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "matchbook/types.hpp"

namespace matchbook {

// A single-instrument limit order book with price-time priority.
// Not thread safe: one book is meant to be driven from one thread.
class OrderBook {
public:
    OrderBook() = default;

    // Match against the book, then (for GTC limit orders) rest what is left.
    // Trades are appended to `trades` in the order they happen.
    SubmitResult submit(const Order& order, std::vector<Trade>& trades);

    // Remove a resting order. Returns false if the id is not on the book.
    bool cancel(OrderId id);

    // Change a resting order. Reducing the quantity at the same price keeps the
    // queue position. Anything else is a cancel and a fresh submit, so the order
    // goes to the back of the queue and may trade if the new price crosses.
    SubmitResult modify(OrderId id, Price new_price, Qty new_qty, std::vector<Trade>& trades);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    // Up to `levels` price levels from the best price outward.
    std::vector<DepthLevel> depth(Side side, std::size_t levels) const;

    std::optional<RestingOrder> find(OrderId id) const;
    std::size_t order_count() const { return index_.size(); }

    // Walks every internal structure and returns a description of the first
    // inconsistency, or an empty string if all is well. Used by the tests.
    std::string validate() const;

private:
    struct Entry {
        OrderId id;
        Qty qty;
    };
    using Queue = std::list<Entry>;
    struct Level {
        Queue queue;
        Qty total = 0;
    };
    using BidMap = std::map<Price, Level, std::greater<Price>>;
    using AskMap = std::map<Price, Level>;
    struct Locator {
        Side side;
        Price price;
        Queue::iterator it;
    };

    template <class Map>
    Qty match(Map& book, const Order& taker, Price limit, std::vector<Trade>& trades);
    template <class Map>
    static bool can_fill(const Map& book, Side taker, Price limit, Qty want);
    template <class Map>
    static std::vector<DepthLevel> snapshot(const Map& book, std::size_t levels);
    template <class Map>
    void erase_from(Map& book, const Locator& loc);
    void rest(const Order& order, Qty qty);

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<OrderId, Locator> index_;
};

}  // namespace matchbook
