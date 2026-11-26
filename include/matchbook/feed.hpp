#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <variant>
#include <vector>

#include "matchbook/types.hpp"

namespace matchbook {

using SymbolId = std::uint32_t;

// The market data feed is a stream of small messages. Each symbol has its own
// sequence number that goes up by one for every message, so a listener can tell
// when it has missed something.

// The total quantity resting at one price changed. qty 0 means the level is gone.
struct LevelUpdate {
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;
    friend bool operator==(const LevelUpdate&, const LevelUpdate&) = default;
};

// The best bid and ask changed. A quantity of 0 means that side is empty.
struct TopOfBook {
    Price bid_price = 0;
    Qty bid_qty = 0;
    Price ask_price = 0;
    Qty ask_qty = 0;
    friend bool operator==(const TopOfBook&, const TopOfBook&) = default;
};

// One trade happened.
struct TradePrint {
    OrderId taker = 0;
    OrderId maker = 0;
    Side taker_side = Side::Buy;
    Price price = 0;
    Qty qty = 0;
    friend bool operator==(const TradePrint&, const TradePrint&) = default;
};

struct FeedMessage {
    std::uint64_t seq = 0;
    SymbolId symbol = 0;
    std::variant<LevelUpdate, TopOfBook, TradePrint> body;
};

// The whole book for one symbol as of sequence number `seq`. A listener that joins
// late asks for this, then applies every message with a higher seq.
struct BookSnapshot {
    SymbolId symbol = 0;
    std::uint64_t seq = 0;
    std::vector<DepthLevel> bids;  // best first
    std::vector<DepthLevel> asks;  // best first
};

// What a late-joining listener keeps: it rebuilds the book from a snapshot and
// the messages after it. Used by the tests and as the reference for how to read the feed.
class BookMirror {
public:
    enum class Apply { Applied, Stale, Gap };

    void load(const BookSnapshot& snap);

    // Stale: seq is not newer than what we have, ignored. Gap: a message is
    // missing, so the mirror is out of date and should load a new snapshot.
    Apply apply(const FeedMessage& msg);

    std::uint64_t seq() const { return seq_; }
    std::vector<DepthLevel> bids() const;
    std::vector<DepthLevel> asks() const;
    std::optional<TopOfBook> top() const { return top_; }
    std::optional<TradePrint> last_trade() const { return last_trade_; }
    std::uint64_t trade_count() const { return trades_; }

private:
    std::uint64_t seq_ = 0;
    std::map<Price, Qty, std::greater<Price>> bids_;
    std::map<Price, Qty> asks_;
    std::optional<TopOfBook> top_;
    std::optional<TradePrint> last_trade_;
    std::uint64_t trades_ = 0;
};

}  // namespace matchbook
