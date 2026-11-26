#pragma once

// A deliberately slow and simple order book used to check the real one.
// Everything is a flat vector that is scanned on every operation. It follows
// the same rules as matchbook::OrderBook but shares no code with it.

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <vector>

#include "matchbook/types.hpp"

namespace matchbook::reference {

class ReferenceBook {
public:
    SubmitResult submit(const Order& o, std::vector<Trade>& trades) {
        if (o.qty <= 0) return rejected(RejectReason::BadQuantity);
        if (o.type == OrderType::Limit && o.price <= 0) return rejected(RejectReason::BadPrice);
        if (find_index(o.id)) return rejected(RejectReason::DuplicateId);

        const bool buy = o.side == Side::Buy;
        Price limit = o.price;
        if (o.type == OrderType::Market) {
            limit = buy ? std::numeric_limits<Price>::max() : std::numeric_limits<Price>::min();
        }
        auto crosses = [&](const Resting& r) {
            return r.side != o.side && (buy ? r.price <= limit : r.price >= limit);
        };

        if (o.tif == TimeInForce::FOK) {
            Qty avail = 0;
            for (const Resting& r : orders_) {
                if (crosses(r)) avail += r.qty;
            }
            if (avail < o.qty) return SubmitResult{Status::Cancelled, RejectReason::None, 0, o.qty};
        }

        Qty left = o.qty;
        while (left > 0) {
            int best = -1;
            for (int i = 0; i < static_cast<int>(orders_.size()); ++i) {
                const Resting& r = orders_[i];
                if (!crosses(r)) continue;
                if (best < 0) {
                    best = i;
                    continue;
                }
                const Resting& b = orders_[best];
                const bool better = buy ? r.price < b.price : r.price > b.price;
                if (better || (r.price == b.price && r.seq < b.seq)) best = i;
            }
            if (best < 0) break;
            Resting& m = orders_[best];
            const Qty q = std::min(left, m.qty);
            trades.push_back(Trade{o.id, m.id, o.side, m.price, q});
            m.qty -= q;
            left -= q;
            if (m.qty == 0) orders_.erase(orders_.begin() + best);
        }

        const Qty filled = o.qty - left;
        if (left == 0) return SubmitResult{Status::Filled, RejectReason::None, filled, 0};
        if (o.type == OrderType::Limit && o.tif == TimeInForce::GTC) {
            orders_.push_back(Resting{o.id, o.side, o.price, left, next_seq_++});
            return SubmitResult{Status::Resting, RejectReason::None, filled, left};
        }
        return SubmitResult{Status::Cancelled, RejectReason::None, filled, left};
    }

    bool cancel(OrderId id) {
        auto i = find_index(id);
        if (!i) return false;
        orders_.erase(orders_.begin() + *i);
        return true;
    }

    SubmitResult modify(OrderId id, Price price, Qty qty, std::vector<Trade>& trades) {
        auto i = find_index(id);
        if (!i) return rejected(RejectReason::UnknownId);
        if (qty <= 0) return rejected(RejectReason::BadQuantity);
        if (price <= 0) return rejected(RejectReason::BadPrice);
        Resting& r = orders_[*i];
        if (price == r.price && qty <= r.qty) {
            r.qty = qty;
            return SubmitResult{Status::Resting, RejectReason::None, 0, qty};
        }
        const Side side = r.side;
        orders_.erase(orders_.begin() + *i);
        return submit(limit_order(id, side, price, qty), trades);
    }

    std::optional<Price> best_bid() const { return best(Side::Buy); }
    std::optional<Price> best_ask() const { return best(Side::Sell); }

    std::vector<DepthLevel> depth(Side side, std::size_t levels) const {
        std::map<Price, DepthLevel> agg;
        for (const Resting& r : orders_) {
            if (r.side != side) continue;
            DepthLevel& l = agg[r.price];
            l.price = r.price;
            l.qty += r.qty;
            ++l.orders;
        }
        std::vector<DepthLevel> out;
        for (const auto& [p, l] : agg) out.push_back(l);
        if (side == Side::Buy) std::reverse(out.begin(), out.end());
        if (out.size() > levels) out.resize(levels);
        return out;
    }

    std::optional<RestingOrder> find(OrderId id) const {
        auto i = find_index(id);
        if (!i) return std::nullopt;
        const Resting& r = orders_[*i];
        return RestingOrder{r.id, r.side, r.price, r.qty};
    }

    std::size_t order_count() const { return orders_.size(); }

private:
    struct Resting {
        OrderId id;
        Side side;
        Price price;
        Qty qty;
        std::uint64_t seq;
    };

    static SubmitResult rejected(RejectReason r) { return SubmitResult{Status::Rejected, r, 0, 0}; }

    std::optional<std::size_t> find_index(OrderId id) const {
        for (std::size_t i = 0; i < orders_.size(); ++i) {
            if (orders_[i].id == id) return i;
        }
        return std::nullopt;
    }

    std::optional<Price> best(Side side) const {
        std::optional<Price> out;
        for (const Resting& r : orders_) {
            if (r.side != side) continue;
            if (!out || (side == Side::Buy ? r.price > *out : r.price < *out)) out = r.price;
        }
        return out;
    }

    std::vector<Resting> orders_;
    std::uint64_t next_seq_ = 0;
};

}  // namespace matchbook::reference
