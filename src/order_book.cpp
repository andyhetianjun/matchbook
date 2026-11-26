#include "matchbook/order_book.hpp"

#include <algorithm>
#include <limits>
#include <sstream>

namespace matchbook {

namespace {

// Does a resting price satisfy the taker's limit?
bool crosses(Side taker, Price resting, Price limit) {
    return taker == Side::Buy ? resting <= limit : resting >= limit;
}

// A market order is a limit order with a limit that every price satisfies.
Price effective_limit(const Order& o) {
    if (o.type == OrderType::Limit) return o.price;
    return o.side == Side::Buy ? std::numeric_limits<Price>::max()
                               : std::numeric_limits<Price>::min();
}

}  // namespace

template <class Map>
Qty OrderBook::match(Map& book, const Order& taker, Price limit, std::vector<Trade>& trades) {
    Qty remaining = taker.qty;
    while (remaining > 0 && !book.empty()) {
        auto level_it = book.begin();
        if (!crosses(taker.side, level_it->first, limit)) break;
        Level& level = level_it->second;
        while (remaining > 0 && !level.queue.empty()) {
            Entry& maker = level.queue.front();
            const Qty q = std::min(remaining, maker.qty);
            trades.push_back(Trade{taker.id, maker.id, taker.side, level_it->first, q});
            maker.qty -= q;
            level.total -= q;
            remaining -= q;
            if (maker.qty == 0) {
                index_.erase(maker.id);
                level.queue.pop_front();
            }
        }
        if (level.queue.empty()) book.erase(level_it);
    }
    return taker.qty - remaining;
}

// Is there enough crossing liquidity to fill `want` completely? Read-only.
template <class Map>
bool OrderBook::can_fill(const Map& book, Side taker, Price limit, Qty want) {
    Qty available = 0;
    for (const auto& [price, level] : book) {
        if (!crosses(taker, price, limit)) break;
        available += level.total;
        if (available >= want) return true;
    }
    return false;
}

template <class Map>
std::vector<DepthLevel> OrderBook::snapshot(const Map& book, std::size_t levels) {
    std::vector<DepthLevel> out;
    for (const auto& [price, level] : book) {
        if (out.size() == levels) break;
        out.push_back(
            DepthLevel{price, level.total, static_cast<std::uint32_t>(level.queue.size())});
    }
    return out;
}

template <class Map>
void OrderBook::erase_from(Map& book, const Locator& loc) {
    auto level_it = book.find(loc.price);
    Level& level = level_it->second;
    level.total -= loc.it->qty;
    level.queue.erase(loc.it);
    if (level.queue.empty()) book.erase(level_it);
}

void OrderBook::rest(const Order& order, Qty qty) {
    auto place = [&](auto& book) {
        Level& level = book[order.price];
        level.queue.push_back(Entry{order.id, qty});
        level.total += qty;
        index_[order.id] = Locator{order.side, order.price, std::prev(level.queue.end())};
    };
    if (order.side == Side::Buy) {
        place(bids_);
    } else {
        place(asks_);
    }
}

SubmitResult OrderBook::submit(const Order& order, std::vector<Trade>& trades) {
    auto reject = [](RejectReason r) { return SubmitResult{Status::Rejected, r, 0, 0}; };
    if (order.qty <= 0) return reject(RejectReason::BadQuantity);
    if (order.type == OrderType::Limit && order.price <= 0) return reject(RejectReason::BadPrice);
    if (index_.contains(order.id)) return reject(RejectReason::DuplicateId);

    const Price limit = effective_limit(order);
    const bool buy = order.side == Side::Buy;

    if (order.tif == TimeInForce::FOK) {
        const bool ok = buy ? can_fill(asks_, order.side, limit, order.qty)
                            : can_fill(bids_, order.side, limit, order.qty);
        if (!ok) return SubmitResult{Status::Cancelled, RejectReason::None, 0, order.qty};
    }

    const Qty filled =
        buy ? match(asks_, order, limit, trades) : match(bids_, order, limit, trades);
    const Qty left = order.qty - filled;
    if (left == 0) return SubmitResult{Status::Filled, RejectReason::None, filled, 0};
    if (order.type == OrderType::Limit && order.tif == TimeInForce::GTC) {
        rest(order, left);
        return SubmitResult{Status::Resting, RejectReason::None, filled, left};
    }
    return SubmitResult{Status::Cancelled, RejectReason::None, filled, left};
}

bool OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    const Locator loc = it->second;
    index_.erase(it);
    if (loc.side == Side::Buy) {
        erase_from(bids_, loc);
    } else {
        erase_from(asks_, loc);
    }
    return true;
}

SubmitResult OrderBook::modify(OrderId id, Price new_price, Qty new_qty,
                               std::vector<Trade>& trades) {
    auto reject = [](RejectReason r) { return SubmitResult{Status::Rejected, r, 0, 0}; };
    auto it = index_.find(id);
    if (it == index_.end()) return reject(RejectReason::UnknownId);
    if (new_qty <= 0) return reject(RejectReason::BadQuantity);
    if (new_price <= 0) return reject(RejectReason::BadPrice);

    const Locator loc = it->second;
    if (new_price == loc.price && new_qty <= loc.it->qty) {
        // Smaller (or same) size at the same price: keep the queue position.
        const Qty delta = loc.it->qty - new_qty;
        loc.it->qty = new_qty;
        auto shrink = [&](auto& book) { book.find(loc.price)->second.total -= delta; };
        if (loc.side == Side::Buy) {
            shrink(bids_);
        } else {
            shrink(asks_);
        }
        return SubmitResult{Status::Resting, RejectReason::None, 0, new_qty};
    }
    const Side side = loc.side;
    cancel(id);
    return submit(limit_order(id, side, new_price, new_qty), trades);
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::vector<DepthLevel> OrderBook::depth(Side side, std::size_t levels) const {
    return side == Side::Buy ? snapshot(bids_, levels) : snapshot(asks_, levels);
}

std::optional<RestingOrder> OrderBook::find(OrderId id) const {
    auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    return RestingOrder{id, it->second.side, it->second.price, it->second.it->qty};
}

std::string OrderBook::validate() const {
    std::ostringstream err;
    std::size_t seen = 0;
    auto check = [&](const auto& book, Side side) {
        for (const auto& [price, level] : book) {
            if (level.queue.empty()) {
                err << "empty level at " << price << "; ";
                continue;
            }
            Qty sum = 0;
            for (auto it = level.queue.begin(); it != level.queue.end(); ++it) {
                ++seen;
                sum += it->qty;
                if (it->qty <= 0) err << "non-positive qty on order " << it->id << "; ";
                auto idx = index_.find(it->id);
                if (idx == index_.end()) {
                    err << "order " << it->id << " missing from index; ";
                } else if (idx->second.side != side || idx->second.price != price ||
                           idx->second.it != it) {
                    err << "index entry for order " << it->id << " is wrong; ";
                }
            }
            if (sum != level.total) err << "level total mismatch at " << price << "; ";
        }
    };
    check(bids_, Side::Buy);
    check(asks_, Side::Sell);
    if (seen != index_.size())
        err << "index has " << index_.size() << " ids, book has " << seen << "; ";
    if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first) {
        err << "book is crossed; ";
    }
    return err.str();
}

}  // namespace matchbook
