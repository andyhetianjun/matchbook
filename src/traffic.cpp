#include "matchbook/traffic.hpp"

namespace matchbook {

namespace {
constexpr std::size_t kRecentSize = 256;
constexpr Price kMinMid = 20;
}  // namespace

TrafficGen::TrafficGen(Exchange& ex, const Config& cfg)
    : ex_(ex), cfg_(cfg), rng_(cfg.seed), mid_(ex.symbol_count(), cfg.start_mid) {
    recent_.reserve(kRecentSize);
}

std::uint64_t TrafficGen::run(std::size_t n) {
    const std::uint64_t before = ex_.ledger().trade_count();
    auto pick = [&](std::uint64_t k) { return rng_() % k; };

    for (std::size_t i = 0; i < n; ++i) {
        const auto sym = static_cast<SymbolId>(pick(mid_.size()));
        Price& mid = mid_[sym];
        if (pick(20) == 0) mid = std::max<Price>(kMinMid, mid + (pick(2) ? 1 : -1));

        const auto account = cfg_.first_account + static_cast<Account>(pick(cfg_.accounts));
        const Side side = pick(2) ? Side::Buy : Side::Sell;
        const Price away = static_cast<Price>(pick(5));  // 0 to 4 ticks
        const Price passive = side == Side::Buy ? mid - 1 - away : mid + 1 + away;
        const Price crossing = side == Side::Buy ? mid + 1 + away : mid - 1 - away;
        const auto qty = static_cast<Qty>(1 + pick(20));
        const auto roll = pick(100);

        NewOrder o{account, sym, side, OrderType::Limit, TimeInForce::GTC, passive, qty};
        if (roll < 62) {
            // passive limit order
        } else if (roll < 75) {
            o.price = crossing;
        } else if (roll < 83) {
            o.price = crossing;
            o.tif = TimeInForce::IOC;
        } else if (roll < 85) {
            o.type = OrderType::Market;
            o.tif = TimeInForce::IOC;
            o.price = 0;
        } else if (roll < 86) {
            o.price = crossing;
            o.tif = TimeInForce::FOK;
        } else {
            // Cancel (or, for a few, modify) something recent. It may be gone already.
            if (recent_.empty()) continue;
            const Recent& r = recent_[pick(recent_.size())];
            if (roll < 96) {
                ex_.cancel(r.account, r.id);
            } else {
                ex_.modify(r.account, r.id, mid + static_cast<Price>(pick(9)) - 4,
                           static_cast<Qty>(1 + pick(20)));
            }
            continue;
        }

        const Placed p = ex_.submit(o);
        if (p.result.status == Status::Resting) {
            if (recent_.size() < kRecentSize) {
                recent_.push_back({p.id, account});
            } else {
                recent_[recent_next_] = {p.id, account};
                recent_next_ = (recent_next_ + 1) % kRecentSize;
            }
        }
    }
    return ex_.ledger().trade_count() - before;
}

}  // namespace matchbook
