#include "matchbook/ledger.hpp"

#include <sstream>

namespace matchbook {

void Ledger::record(SymbolId symbol, Account buyer, Account seller, Price price, Qty qty) {
    const LedgerEntry e{next_trade_no_++, symbol, buyer, seller, price, qty};
    cash_[buyer] -= e.cash();
    cash_[seller] += e.cash();
    positions_[{buyer, symbol}] += qty;
    positions_[{seller, symbol}] -= qty;
    traded_cash_ += e.cash();
    traded_qty_[symbol] += qty;
    entries_.push_back(e);
    if (entries_.size() > max_entries_) {
        entries_.pop_front();
        ++dropped_;
    }
}

std::int64_t Ledger::cash(Account a) const {
    auto it = cash_.find(a);
    return it == cash_.end() ? 0 : it->second;
}

Qty Ledger::position(Account a, SymbolId symbol) const {
    auto it = positions_.find({a, symbol});
    return it == positions_.end() ? 0 : it->second;
}

Qty Ledger::traded_qty(SymbolId symbol) const {
    auto it = traded_qty_.find(symbol);
    return it == traded_qty_.end() ? 0 : it->second;
}

std::string Ledger::check() const {
    std::ostringstream err;

    std::int64_t cash_sum = 0;
    for (const auto& [a, c] : cash_) cash_sum += c;
    if (cash_sum != 0) err << "cash does not sum to zero (" << cash_sum << "); ";

    std::map<SymbolId, Qty> pos_sum;
    for (const auto& [key, q] : positions_) pos_sum[key.second] += q;
    for (const auto& [sym, q] : pos_sum) {
        if (q != 0) err << "positions in symbol " << sym << " do not sum to zero (" << q << "); ";
    }

    // The sum of everything that was bought equals what was traded.
    std::map<SymbolId, Qty> bought;
    for (const auto& [key, q] : positions_) {
        if (q > 0) bought[key.second] += q;
    }
    // (net long positions can be smaller than traded quantity when accounts trade
    // with themselves or both ways, so only check the inequality)
    for (const auto& [sym, q] : bought) {
        if (q > traded_qty(sym))
            err << "net long exceeds traded quantity in symbol " << sym << "; ";
    }

    // Rebuild the balances from the entries, if we still have all of them.
    if (dropped_ == 0) {
        std::map<Account, std::int64_t> c2;
        std::map<std::pair<Account, SymbolId>, Qty> p2;
        std::int64_t notional = 0;
        std::uint64_t expect_no = 1;
        for (const auto& e : entries_) {
            if (e.trade_no != expect_no++) err << "trade numbers are not consecutive; ";
            c2[e.buyer] -= e.cash();
            c2[e.seller] += e.cash();
            p2[{e.buyer, e.symbol}] += e.qty;
            p2[{e.seller, e.symbol}] -= e.qty;
            notional += e.cash();
        }
        auto nonzero = [](auto& m) {
            for (auto it = m.begin(); it != m.end();) {
                it = (it->second == 0) ? m.erase(it) : std::next(it);
            }
        };
        auto c1 = cash_;
        auto p1 = positions_;
        nonzero(c1);
        nonzero(c2);
        nonzero(p1);
        nonzero(p2);
        if (c1 != c2) err << "cash balances differ from the entries; ";
        if (p1 != p2) err << "positions differ from the entries; ";
        if (notional != traded_cash_) err << "traded cash differs from the entries; ";
    }
    return err.str();
}

}  // namespace matchbook
