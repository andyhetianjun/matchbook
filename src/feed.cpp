#include "matchbook/feed.hpp"

namespace matchbook {

void BookMirror::load(const BookSnapshot& snap) {
    seq_ = snap.seq;
    bids_.clear();
    asks_.clear();
    for (const auto& l : snap.bids) bids_[l.price] = l.qty;
    for (const auto& l : snap.asks) asks_[l.price] = l.qty;
    top_.reset();
}

BookMirror::Apply BookMirror::apply(const FeedMessage& msg) {
    if (msg.seq <= seq_) return Apply::Stale;
    if (msg.seq != seq_ + 1) return Apply::Gap;
    seq_ = msg.seq;
    if (const auto* lu = std::get_if<LevelUpdate>(&msg.body)) {
        auto set = [&](auto& side) {
            if (lu->qty == 0) {
                side.erase(lu->price);
            } else {
                side[lu->price] = lu->qty;
            }
        };
        if (lu->side == Side::Buy) {
            set(bids_);
        } else {
            set(asks_);
        }
    } else if (const auto* t = std::get_if<TopOfBook>(&msg.body)) {
        top_ = *t;
    } else if (const auto* tp = std::get_if<TradePrint>(&msg.body)) {
        last_trade_ = *tp;
        ++trades_;
    }
    return Apply::Applied;
}

std::vector<DepthLevel> BookMirror::bids() const {
    std::vector<DepthLevel> out;
    for (const auto& [p, q] : bids_) out.push_back(DepthLevel{p, q, 0});
    return out;
}

std::vector<DepthLevel> BookMirror::asks() const {
    std::vector<DepthLevel> out;
    for (const auto& [p, q] : asks_) out.push_back(DepthLevel{p, q, 0});
    return out;
}

}  // namespace matchbook
