#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>

#include "matchbook/feed.hpp"
#include "matchbook/types.hpp"

namespace matchbook {

using Account = std::uint32_t;

// One trade, written down as who owes what: the buyer owes the seller
// price * qty in cash, and the seller owes the buyer qty of the instrument.
struct LedgerEntry {
    std::uint64_t trade_no = 0;
    SymbolId symbol = 0;
    Account buyer = 0;
    Account seller = 0;
    Price price = 0;
    Qty qty = 0;

    std::int64_t cash() const { return price * qty; }
    friend bool operator==(const LedgerEntry&, const LedgerEntry&) = default;
};

// Records every trade and keeps a running balance per account. Because every trade
// moves cash and quantity in equal and opposite amounts, the totals must always
// come to zero. check() proves it, and also rebuilds the balances from the entries.
class Ledger {
public:
    // `max_entries` limits how many entries are kept in memory (oldest dropped first).
    // Balances are always kept in full.
    explicit Ledger(std::size_t max_entries = SIZE_MAX) : max_entries_(max_entries) {}

    void record(SymbolId symbol, Account buyer, Account seller, Price price, Qty qty);

    std::int64_t cash(Account a) const;
    Qty position(Account a, SymbolId symbol) const;

    std::uint64_t trade_count() const { return next_trade_no_ - 1; }
    std::int64_t traded_cash() const { return traded_cash_; }
    Qty traded_qty(SymbolId symbol) const;
    const std::deque<LedgerEntry>& entries() const { return entries_; }
    std::size_t dropped_entries() const { return dropped_; }
    const std::map<Account, std::int64_t>& cash_balances() const { return cash_; }
    const std::map<std::pair<Account, SymbolId>, Qty>& positions() const { return positions_; }

    // Empty string if everything balances, otherwise a description of what is wrong.
    std::string check() const;

private:
    std::size_t max_entries_;
    std::uint64_t next_trade_no_ = 1;
    std::int64_t traded_cash_ = 0;
    std::size_t dropped_ = 0;
    std::deque<LedgerEntry> entries_;
    std::map<Account, std::int64_t> cash_;
    std::map<std::pair<Account, SymbolId>, Qty> positions_;
    std::map<SymbolId, Qty> traded_qty_;
};

}  // namespace matchbook
