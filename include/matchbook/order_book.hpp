#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
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
    // `reserve_orders` is a hint for how many orders will rest at once, so the
    // book can allocate up front instead of growing as it goes.
    explicit OrderBook(std::size_t reserve_orders = 0) {
        nodes_.reserve(reserve_orders);
        index_.reserve(reserve_orders);
    }

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
    // Resting orders live in one vector ("the pool") and refer to each other by
    // index, so there is no allocation per order and neighbours are close in memory.
    using Slot = std::uint32_t;
    static constexpr Slot kNil = std::numeric_limits<Slot>::max();

    struct Level;
    struct Node {
        OrderId id = 0;
        Qty qty = 0;
        Slot prev = kNil;  // towards the front of the queue
        Slot next = kNil;  // towards the back; also the free-list link
        Level* level = nullptr;
    };
    struct Level {
        Price price = 0;
        Side side = Side::Buy;
        Slot head = kNil;  // oldest order, matched first
        Slot tail = kNil;
        Qty total = 0;
        std::uint32_t count = 0;
    };
    // std::map nodes never move, so a Node can hold a plain Level pointer.
    using BidMap = std::map<Price, Level, std::greater<Price>>;
    using AskMap = std::map<Price, Level>;

    Slot allocate(OrderId id, Qty qty);
    void release(Slot slot);
    static void push_back(std::vector<Node>& nodes, Level& level, Slot slot);
    static void unlink(std::vector<Node>& nodes, Level& level, Slot slot);

    template <class Map>
    Qty match(Map& book, const Order& taker, Price limit, std::vector<Trade>& trades);
    template <class Map>
    static bool can_fill(const Map& book, Side taker, Price limit, Qty want);
    template <class Map>
    static std::vector<DepthLevel> snapshot(const Map& book, std::size_t levels);
    void rest(const Order& order, Qty qty);

    BidMap bids_;
    AskMap asks_;
    std::vector<Node> nodes_;
    Slot free_head_ = kNil;
    std::unordered_map<OrderId, Slot> index_;
};

}  // namespace matchbook
