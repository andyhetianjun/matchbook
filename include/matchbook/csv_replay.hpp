#pragma once

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

#include "matchbook/order_book.hpp"
#include "matchbook/types.hpp"

namespace matchbook {

// One line of input:  action,id,side,type,tif,price,qty
//   NEW,1,BUY,LIMIT,GTC,100,10
//   NEW,2,SELL,MARKET,IOC,,5
//   CANCEL,1
//   MODIFY,1,,,,101,8        (id, new price, new quantity)
struct Command {
    enum class Kind { New, Cancel, Modify };
    Kind kind = Kind::New;
    Order order;      // New
    OrderId id = 0;   // Cancel, Modify
    Price price = 0;  // Modify
    Qty qty = 0;      // Modify
};

struct ParseResult {
    std::optional<Command> command;  // empty for blank lines, comments, the header, or errors
    std::string error;               // non-empty if the line was bad
};

ParseResult parse_line(std::string_view line);

const char* to_string(Side side);
const char* to_string(RejectReason reason);

struct ReplaySummary {
    std::size_t lines_read = 0;
    std::size_t commands = 0;
    std::size_t trades = 0;
    std::size_t rejects = 0;
    std::size_t bad_lines = 0;
};

// Applies every command to `book`. Writes TRADE and REJECT lines to `out`,
// and one "line N: ..." message per unparsable line to `err`.
ReplaySummary replay_csv(std::istream& in, std::ostream& out, std::ostream& err, OrderBook& book);

// Prints the top `levels` price levels of each side.
void print_book(std::ostream& out, const OrderBook& book, std::size_t levels);

}  // namespace matchbook
