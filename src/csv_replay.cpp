#include "matchbook/csv_replay.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <istream>
#include <ostream>
#include <vector>

namespace matchbook {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> split(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = line.find(',', start);
        if (comma == std::string_view::npos) {
            fields.push_back(trim(line.substr(start)));
            return fields;
        }
        fields.push_back(trim(line.substr(start, comma - start)));
        start = comma + 1;
    }
}

std::string upper(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

template <class T>
bool parse_number(std::string_view s, T& out) {
    if (s.empty()) return false;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && end == s.data() + s.size();
}

std::string_view field(const std::vector<std::string_view>& f, std::size_t i) {
    return i < f.size() ? f[i] : std::string_view{};
}

}  // namespace

const char* to_string(Side side) {
    return side == Side::Buy ? "BUY" : "SELL";
}

const char* to_string(RejectReason reason) {
    switch (reason) {
        case RejectReason::None:
            return "None";
        case RejectReason::DuplicateId:
            return "DuplicateId";
        case RejectReason::UnknownId:
            return "UnknownId";
        case RejectReason::BadQuantity:
            return "BadQuantity";
        case RejectReason::BadPrice:
            return "BadPrice";
    }
    return "?";
}

ParseResult parse_line(std::string_view line) {
    line = trim(line);
    if (line.empty() || line.front() == '#') return {};
    const auto f = split(line);
    const std::string action = upper(f[0]);
    if (action == "ACTION") return {};  // header row

    auto fail = [](std::string msg) { return ParseResult{std::nullopt, std::move(msg)}; };

    Command cmd;
    if (action == "NEW") {
        cmd.kind = Command::Kind::New;
        Order& o = cmd.order;
        if (!parse_number(field(f, 1), o.id)) return fail("bad order id");
        const std::string side = upper(field(f, 2));
        if (side == "BUY" || side == "B") {
            o.side = Side::Buy;
        } else if (side == "SELL" || side == "S") {
            o.side = Side::Sell;
        } else {
            return fail("side must be BUY or SELL");
        }
        const std::string type = upper(field(f, 3));
        if (type == "LIMIT") {
            o.type = OrderType::Limit;
        } else if (type == "MARKET") {
            o.type = OrderType::Market;
        } else {
            return fail("type must be LIMIT or MARKET");
        }
        const std::string tif = upper(field(f, 4));
        if (tif.empty() || tif == "GTC") {
            o.tif = TimeInForce::GTC;
        } else if (tif == "IOC") {
            o.tif = TimeInForce::IOC;
        } else if (tif == "FOK") {
            o.tif = TimeInForce::FOK;
        } else {
            return fail("tif must be GTC, IOC or FOK");
        }
        if (o.type == OrderType::Limit && !parse_number(field(f, 5), o.price)) {
            return fail("bad price");
        }
        if (!parse_number(field(f, 6), o.qty)) return fail("bad quantity");
    } else if (action == "CANCEL") {
        cmd.kind = Command::Kind::Cancel;
        if (!parse_number(field(f, 1), cmd.id)) return fail("bad order id");
    } else if (action == "MODIFY") {
        cmd.kind = Command::Kind::Modify;
        if (!parse_number(field(f, 1), cmd.id)) return fail("bad order id");
        if (!parse_number(field(f, 5), cmd.price)) return fail("bad price");
        if (!parse_number(field(f, 6), cmd.qty)) return fail("bad quantity");
    } else {
        return fail("action must be NEW, CANCEL or MODIFY");
    }
    return ParseResult{cmd, {}};
}

ReplaySummary replay_csv(std::istream& in, std::ostream& out, std::ostream& err, OrderBook& book) {
    ReplaySummary sum;
    std::vector<Trade> trades;
    std::string line;
    while (std::getline(in, line)) {
        ++sum.lines_read;
        const ParseResult parsed = parse_line(line);
        if (!parsed.error.empty()) {
            ++sum.bad_lines;
            err << "line " << sum.lines_read << ": " << parsed.error << '\n';
            continue;
        }
        if (!parsed.command) continue;
        ++sum.commands;
        const Command& cmd = *parsed.command;
        trades.clear();
        OrderId id = cmd.id;
        SubmitResult result{Status::Filled, RejectReason::None, 0, 0};
        switch (cmd.kind) {
            case Command::Kind::New:
                id = cmd.order.id;
                result = book.submit(cmd.order, trades);
                break;
            case Command::Kind::Cancel:
                if (!book.cancel(cmd.id)) {
                    result = SubmitResult{Status::Rejected, RejectReason::UnknownId, 0, 0};
                }
                break;
            case Command::Kind::Modify:
                result = book.modify(cmd.id, cmd.price, cmd.qty, trades);
                break;
        }
        for (const Trade& t : trades) {
            out << "TRADE," << t.taker_id << ',' << t.maker_id << ',' << to_string(t.taker_side)
                << ',' << t.price << ',' << t.qty << '\n';
        }
        sum.trades += trades.size();
        if (result.status == Status::Rejected) {
            ++sum.rejects;
            out << "REJECT," << id << ',' << to_string(result.reason) << '\n';
        }
    }
    return sum;
}

void print_book(std::ostream& out, const OrderBook& book, std::size_t levels) {
    auto asks = book.depth(Side::Sell, levels);
    std::reverse(asks.begin(), asks.end());  // highest ask on top, so the spread is in the middle
    out << "BOOK\n";
    for (const DepthLevel& l : asks) {
        out << "  ASK " << l.price << " x " << l.qty << " (" << l.orders << " orders)\n";
    }
    out << "  ---\n";
    for (const DepthLevel& l : book.depth(Side::Buy, levels)) {
        out << "  BID " << l.price << " x " << l.qty << " (" << l.orders << " orders)\n";
    }
}

}  // namespace matchbook
