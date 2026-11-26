#pragma once

#include <cstdint>
#include <limits>

namespace matchbook {

using OrderId = std::uint64_t;
using Price = std::int64_t;  // in ticks; there are no floating point prices anywhere
using Qty = std::int64_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };
// GTC: rest on the book until filled or cancelled.
// IOC: fill what you can right now, cancel the rest.
// FOK: fill the whole quantity right now or do nothing at all.
enum class TimeInForce : std::uint8_t { GTC, IOC, FOK };

constexpr Side opposite(Side s) {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

struct Order {
    OrderId id = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GTC;
    Price price = 0;  // ignored for market orders
    Qty qty = 0;

    friend bool operator==(const Order&, const Order&) = default;
};

constexpr Order limit_order(OrderId id, Side side, Price price, Qty qty,
                            TimeInForce tif = TimeInForce::GTC) {
    return Order{id, side, OrderType::Limit, tif, price, qty};
}

// A market order never rests, so GTC behaves like IOC for it.
constexpr Order market_order(OrderId id, Side side, Qty qty, TimeInForce tif = TimeInForce::IOC) {
    return Order{id, side, OrderType::Market, tif, 0, qty};
}

struct Trade {
    OrderId taker_id = 0;
    OrderId maker_id = 0;
    Side taker_side = Side::Buy;
    Price price = 0;  // always the resting (maker) order's price
    Qty qty = 0;

    friend bool operator==(const Trade&, const Trade&) = default;
};

enum class Status : std::uint8_t {
    Resting,    // some or all of the order is now on the book
    Filled,     // completely filled, nothing left
    Cancelled,  // the unfilled part was dropped (IOC, FOK, market order)
    Rejected,   // invalid, nothing happened
};

enum class RejectReason : std::uint8_t { None, DuplicateId, UnknownId, BadQuantity, BadPrice };

struct SubmitResult {
    Status status = Status::Rejected;
    RejectReason reason = RejectReason::None;
    Qty filled = 0;
    // Resting: quantity on the book. Cancelled: quantity that was dropped. Otherwise 0.
    Qty remaining = 0;

    friend bool operator==(const SubmitResult&, const SubmitResult&) = default;
};

struct DepthLevel {
    Price price = 0;
    Qty qty = 0;
    std::uint32_t orders = 0;

    friend bool operator==(const DepthLevel&, const DepthLevel&) = default;
};

struct RestingOrder {
    OrderId id = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;

    friend bool operator==(const RestingOrder&, const RestingOrder&) = default;
};

}  // namespace matchbook
