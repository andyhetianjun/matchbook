#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "matchbook/feed.hpp"
#include "matchbook/ledger.hpp"
#include "matchbook/order_book.hpp"

namespace matchbook {

struct NewOrder {
    Account account = 0;
    SymbolId symbol = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GTC;
    Price price = 0;
    Qty qty = 0;
};

struct Placed {
    OrderId id = 0;
    SubmitResult result;
};

// Many order books at once, one per symbol. The exchange gives every order a
// unique id, remembers who owns each resting order, publishes the market data feed
// and writes every trade into the ledger.
// Not thread safe: drive it from one thread.
class Exchange {
public:
    using Listener = std::function<void(const FeedMessage&)>;

    explicit Exchange(std::size_t ledger_max_entries = SIZE_MAX) : ledger_(ledger_max_entries) {}

    SymbolId add_symbol(const std::string& name);
    std::optional<SymbolId> find_symbol(std::string_view name) const;
    const std::string& symbol_name(SymbolId id) const { return symbols_.at(id)->name; }
    std::size_t symbol_count() const { return symbols_.size(); }

    // Listeners see every message, in order, as it is published.
    void subscribe(Listener l) { listeners_.push_back(std::move(l)); }

    Placed submit(const NewOrder& order);
    // Only the account that placed the order may cancel or modify it.
    bool cancel(Account account, OrderId id);
    Placed modify(Account account, OrderId id, Price new_price, Qty new_qty);

    // The whole book for one symbol, tied to a sequence number (see BookSnapshot).
    BookSnapshot snapshot(SymbolId symbol, std::size_t levels = SIZE_MAX) const;
    std::uint64_t seq(SymbolId symbol) const { return symbols_.at(symbol)->seq; }

    const OrderBook& book(SymbolId symbol) const { return symbols_.at(symbol)->book; }
    const Ledger& ledger() const { return ledger_; }
    std::optional<Account> owner(OrderId id) const;

private:
    struct Symbol {
        std::string name;
        OrderBook book;
        std::uint64_t seq = 0;
        TopOfBook top;
    };
    struct Owner {
        Account account;
        SymbolId symbol;
    };

    void publish(Symbol& s, SymbolId id, std::variant<LevelUpdate, TopOfBook, TradePrint> body);
    // After an operation: record trades, tidy ownership, and publish the feed messages.
    void settle_and_publish(SymbolId id, OrderId taker, Account taker_account, Side taker_side,
                            const std::vector<Trade>& trades,
                            std::vector<std::pair<Side, Price>> touched);

    // Symbols are boxed so references to them stay valid as more are added.
    std::vector<std::unique_ptr<Symbol>> symbols_;
    std::unordered_map<std::string, SymbolId> by_name_;
    std::unordered_map<OrderId, Owner> owners_;  // resting orders only
    std::vector<Listener> listeners_;
    Ledger ledger_;
    OrderId next_id_ = 1;
    std::vector<Trade> scratch_;
};

}  // namespace matchbook
