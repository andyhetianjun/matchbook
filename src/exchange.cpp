#include "matchbook/exchange.hpp"

#include <algorithm>
#include <stdexcept>

namespace matchbook {

SymbolId Exchange::add_symbol(const std::string& name) {
    if (by_name_.count(name)) throw std::invalid_argument("duplicate symbol " + name);
    const auto id = static_cast<SymbolId>(symbols_.size());
    auto s = std::make_unique<Symbol>();
    s->name = name;
    symbols_.push_back(std::move(s));
    by_name_[name] = id;
    return id;
}

std::optional<SymbolId> Exchange::find_symbol(std::string_view name) const {
    auto it = by_name_.find(std::string(name));
    if (it == by_name_.end()) return std::nullopt;
    return it->second;
}

std::optional<Account> Exchange::owner(OrderId id) const {
    auto it = owners_.find(id);
    if (it == owners_.end()) return std::nullopt;
    return it->second.account;
}

void Exchange::publish(Symbol& s, SymbolId id,
                       std::variant<LevelUpdate, TopOfBook, TradePrint> body) {
    FeedMessage m{++s.seq, id, std::move(body)};
    for (auto& l : listeners_) l(m);
}

void Exchange::settle_and_publish(SymbolId id, OrderId taker, Account taker_account,
                                  Side taker_side, const std::vector<Trade>& trades,
                                  std::vector<std::pair<Side, Price>> touched) {
    Symbol& s = *symbols_[id];
    for (const Trade& t : trades) {
        // The maker is a resting order, so its owner is in the table.
        const Account maker_account = owners_.at(t.maker_id).account;
        const bool taker_buys = t.taker_side == Side::Buy;
        ledger_.record(id, taker_buys ? taker_account : maker_account,
                       taker_buys ? maker_account : taker_account, t.price, t.qty);
        publish(s, id, TradePrint{t.taker_id, t.maker_id, t.taker_side, t.price, t.qty});
        touched.emplace_back(opposite(taker_side), t.price);
        if (!s.book.find(t.maker_id)) owners_.erase(t.maker_id);
    }
    if (!s.book.find(taker)) owners_.erase(taker);

    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    for (const auto& [side, price] : touched) {
        publish(s, id, LevelUpdate{side, price, s.book.level_at(side, price).qty});
    }

    TopOfBook now;
    if (auto d = s.book.depth(Side::Buy, 1); !d.empty()) {
        now.bid_price = d[0].price;
        now.bid_qty = d[0].qty;
    }
    if (auto d = s.book.depth(Side::Sell, 1); !d.empty()) {
        now.ask_price = d[0].price;
        now.ask_qty = d[0].qty;
    }
    if (!(now == s.top)) {
        s.top = now;
        publish(s, id, now);
    }
}

Placed Exchange::submit(const NewOrder& n) {
    Symbol& s = *symbols_.at(n.symbol);
    const OrderId id = next_id_++;
    Order o{id, n.side, n.type, n.tif, n.price, n.qty};
    scratch_.clear();
    // Taker ownership is needed before trades are settled (a self-trade looks up both).
    const SubmitResult r = s.book.submit(o, scratch_);
    if (r.status == Status::Rejected) return {id, r};
    if (r.status == Status::Resting) owners_[id] = Owner{n.account, n.symbol};

    std::vector<std::pair<Side, Price>> touched;
    if (r.status == Status::Resting) touched.emplace_back(n.side, n.price);
    settle_and_publish(n.symbol, id, n.account, n.side, scratch_, std::move(touched));
    return {id, r};
}

bool Exchange::cancel(Account account, OrderId id) {
    auto it = owners_.find(id);
    if (it == owners_.end() || it->second.account != account) return false;
    const SymbolId sym = it->second.symbol;
    Symbol& s = *symbols_[sym];
    const auto resting = s.book.find(id);
    s.book.cancel(id);
    scratch_.clear();
    settle_and_publish(sym, id, account, resting->side, scratch_,
                       {{resting->side, resting->price}});
    return true;
}

Placed Exchange::modify(Account account, OrderId id, Price new_price, Qty new_qty) {
    auto it = owners_.find(id);
    if (it == owners_.end() || it->second.account != account) {
        return {id, SubmitResult{Status::Rejected, RejectReason::UnknownId, 0, 0}};
    }
    const SymbolId sym = it->second.symbol;
    Symbol& s = *symbols_[sym];
    const auto before = *s.book.find(id);
    scratch_.clear();
    const SubmitResult r = s.book.modify(id, new_price, new_qty, scratch_);
    if (r.status == Status::Rejected) return {id, r};

    std::vector<std::pair<Side, Price>> touched{{before.side, before.price}};
    if (r.status == Status::Resting) touched.emplace_back(before.side, new_price);
    settle_and_publish(sym, id, account, before.side, scratch_, std::move(touched));
    return {id, r};
}

BookSnapshot Exchange::snapshot(SymbolId symbol, std::size_t levels) const {
    const Symbol& s = *symbols_.at(symbol);
    return BookSnapshot{symbol, s.seq, s.book.depth(Side::Buy, levels),
                        s.book.depth(Side::Sell, levels)};
}

}  // namespace matchbook
