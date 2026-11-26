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

OrderBook::Slot OrderBook::allocate(OrderId id, Qty qty) {
    Slot slot;
    if (free_head_ != kNil) {
        slot = free_head_;
        free_head_ = nodes_[slot].next;
    } else {
        slot = static_cast<Slot>(nodes_.size());
        nodes_.emplace_back();
    }
    nodes_[slot] = Node{id, qty, kNil, kNil, nullptr};
    return slot;
}

void OrderBook::release(Slot slot) {
    nodes_[slot].next = free_head_;
    free_head_ = slot;
}

void OrderBook::push_back(std::vector<Node>& nodes, Level& level, Slot slot) {
    Node& n = nodes[slot];
    n.level = &level;
    n.prev = level.tail;
    n.next = kNil;
    if (level.tail != kNil) {
        nodes[level.tail].next = slot;
    } else {
        level.head = slot;
    }
    level.tail = slot;
    level.total += n.qty;
    ++level.count;
}

void OrderBook::unlink(std::vector<Node>& nodes, Level& level, Slot slot) {
    Node& n = nodes[slot];
    if (n.prev != kNil) {
        nodes[n.prev].next = n.next;
    } else {
        level.head = n.next;
    }
    if (n.next != kNil) {
        nodes[n.next].prev = n.prev;
    } else {
        level.tail = n.prev;
    }
    level.total -= n.qty;
    --level.count;
}

template <class Map>
Qty OrderBook::match(Map& book, const Order& taker, Price limit, std::vector<Trade>& trades) {
    Qty remaining = taker.qty;
    while (remaining > 0 && !book.empty()) {
        auto level_it = book.begin();
        if (!crosses(taker.side, level_it->first, limit)) break;
        Level& level = level_it->second;
        while (remaining > 0 && level.head != kNil) {
            const Slot slot = level.head;
            Node& maker = nodes_[slot];
            const Qty q = std::min(remaining, maker.qty);
            trades.push_back(Trade{taker.id, maker.id, taker.side, level.price, q});
            maker.qty -= q;
            level.total -= q;
            remaining -= q;
            if (maker.qty == 0) {
                index_.erase(maker.id);
                level.head = maker.next;
                if (maker.next != kNil) {
                    nodes_[maker.next].prev = kNil;
                } else {
                    level.tail = kNil;
                }
                --level.count;
                release(slot);
            }
        }
        if (level.head == kNil) book.erase(level_it);
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
        out.push_back(DepthLevel{price, level.total, level.count});
    }
    return out;
}

void OrderBook::rest(const Order& order, Qty qty) {
    const Slot slot = allocate(order.id, qty);
    auto place = [&](auto& book) {
        Level& level = book[order.price];
        level.price = order.price;
        level.side = order.side;
        push_back(nodes_, level, slot);
    };
    if (order.side == Side::Buy) {
        place(bids_);
    } else {
        place(asks_);
    }
    index_[order.id] = slot;
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
    const Slot slot = it->second;
    index_.erase(it);
    Level& level = *nodes_[slot].level;
    unlink(nodes_, level, slot);
    release(slot);
    if (level.head == kNil) {
        // Only now do we pay for a tree lookup, and only when a price level empties.
        if (level.side == Side::Buy) {
            bids_.erase(level.price);
        } else {
            asks_.erase(level.price);
        }
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

    Node& node = nodes_[it->second];
    Level& level = *node.level;
    if (new_price == level.price && new_qty <= node.qty) {
        // Smaller (or same) size at the same price: keep the queue position.
        level.total -= node.qty - new_qty;
        node.qty = new_qty;
        return SubmitResult{Status::Resting, RejectReason::None, 0, new_qty};
    }
    const Side side = level.side;
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

DepthLevel OrderBook::level_at(Side side, Price price) const {
    auto look = [&](const auto& book) {
        auto it = book.find(price);
        if (it == book.end()) return DepthLevel{price, 0, 0};
        return DepthLevel{price, it->second.total, it->second.count};
    };
    return side == Side::Buy ? look(bids_) : look(asks_);
}

std::optional<RestingOrder> OrderBook::find(OrderId id) const {
    auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    const Node& n = nodes_[it->second];
    return RestingOrder{id, n.level->side, n.level->price, n.qty};
}

std::string OrderBook::validate() const {
    std::ostringstream err;
    std::size_t seen = 0;
    auto check = [&](const auto& book, Side side) {
        for (const auto& [price, level] : book) {
            if (level.head == kNil) {
                err << "empty level at " << price << "; ";
                continue;
            }
            if (level.price != price || level.side != side)
                err << "level fields wrong at " << price << "; ";
            Qty sum = 0;
            std::uint32_t count = 0;
            Slot prev = kNil;
            for (Slot s = level.head; s != kNil; s = nodes_[s].next) {
                const Node& n = nodes_[s];
                ++count;
                sum += n.qty;
                if (n.qty <= 0) err << "non-positive qty on order " << n.id << "; ";
                if (n.prev != prev) err << "broken prev link at order " << n.id << "; ";
                if (n.level != &level) err << "wrong level pointer on order " << n.id << "; ";
                auto idx = index_.find(n.id);
                if (idx == index_.end() || idx->second != s) {
                    err << "order " << n.id << " missing or wrong in index; ";
                }
                prev = s;
                if (count > nodes_.size()) {
                    err << "cycle at level " << price << "; ";
                    break;
                }
            }
            if (prev != level.tail) err << "tail wrong at " << price << "; ";
            if (sum != level.total) err << "level total mismatch at " << price << "; ";
            if (count != level.count) err << "level count mismatch at " << price << "; ";
            seen += count;
        }
    };
    check(bids_, Side::Buy);
    check(asks_, Side::Sell);
    if (seen != index_.size()) {
        err << "index has " << index_.size() << " ids, book has " << seen << "; ";
    }
    std::size_t free_slots = 0;
    for (Slot s = free_head_; s != kNil && free_slots <= nodes_.size(); s = nodes_[s].next) {
        ++free_slots;
    }
    if (free_slots + seen != nodes_.size()) err << "pool slots leaked or double-freed; ";
    if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first) {
        err << "book is crossed; ";
    }
    return err.str();
}

}  // namespace matchbook
