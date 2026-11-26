#include "matchbook/wire.hpp"

#include <cctype>
#include <charconv>
#include <sstream>
#include <vector>

namespace matchbook {

namespace {

const char* side_name(Side s) {
    return s == Side::Buy ? "buy" : "sell";
}

void levels_json(std::ostream& o, const std::vector<DepthLevel>& v) {
    o << '[';
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) o << ',';
        o << '[' << v[i].price << ',' << v[i].qty << ']';
    }
    o << ']';
}

std::vector<std::string_view> split(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        std::size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t' && line[j] != '\r') ++j;
        if (j > i) out.push_back(line.substr(i, j - i));
        i = j;
    }
    return out;
}

template <class T>
bool parse_int(std::string_view s, T& out) {
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
}

// Only plain tokens may be echoed back inside JSON strings.
bool plain(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.'))
            return false;
    }
    return true;
}

const char* status_name(Status s) {
    switch (s) {
        case Status::Resting:
            return "resting";
        case Status::Filled:
            return "filled";
        case Status::Cancelled:
            return "cancelled";
        case Status::Rejected:
            return "rejected";
    }
    return "?";
}

const char* reason_name(RejectReason r) {
    switch (r) {
        case RejectReason::None:
            return "none";
        case RejectReason::DuplicateId:
            return "duplicate id";
        case RejectReason::UnknownId:
            return "unknown id";
        case RejectReason::BadQuantity:
            return "bad quantity";
        case RejectReason::BadPrice:
            return "bad price";
    }
    return "?";
}

}  // namespace

std::string to_json(const Exchange& ex, const FeedMessage& m) {
    std::ostringstream o;
    o << "{\"type\":\"";
    if (const auto* lu = std::get_if<LevelUpdate>(&m.body)) {
        o << "level\",\"symbol\":\"" << ex.symbol_name(m.symbol) << "\",\"seq\":" << m.seq
          << ",\"side\":\"" << side_name(lu->side) << "\",\"price\":" << lu->price
          << ",\"qty\":" << lu->qty;
    } else if (const auto* t = std::get_if<TopOfBook>(&m.body)) {
        o << "top\",\"symbol\":\"" << ex.symbol_name(m.symbol) << "\",\"seq\":" << m.seq
          << ",\"bid\":"
          << (t->bid_qty
                  ? "[" + std::to_string(t->bid_price) + "," + std::to_string(t->bid_qty) + "]"
                  : std::string("null"))
          << ",\"ask\":"
          << (t->ask_qty
                  ? "[" + std::to_string(t->ask_price) + "," + std::to_string(t->ask_qty) + "]"
                  : std::string("null"));
    } else {
        const auto& t2 = std::get<TradePrint>(m.body);
        o << "trade\",\"symbol\":\"" << ex.symbol_name(m.symbol) << "\",\"seq\":" << m.seq
          << ",\"taker\":" << t2.taker << ",\"maker\":" << t2.maker << ",\"side\":\""
          << side_name(t2.taker_side) << "\",\"price\":" << t2.price << ",\"qty\":" << t2.qty;
    }
    o << '}';
    return o.str();
}

std::string to_json(const Exchange& ex, const BookSnapshot& s) {
    std::ostringstream o;
    o << "{\"type\":\"snapshot\",\"symbol\":\"" << ex.symbol_name(s.symbol)
      << "\",\"seq\":" << s.seq << ",\"bids\":";
    levels_json(o, s.bids);
    o << ",\"asks\":";
    levels_json(o, s.asks);
    o << '}';
    return o.str();
}

Protocol::Protocol(Exchange& ex, Sink sink) : ex_(ex), sink_(std::move(sink)) {
    ex_.subscribe([this](const FeedMessage& m) { sink_(to_json(ex_, m)); });
}

void Protocol::reply_error(std::string_view ref, std::string_view reason) {
    std::ostringstream o;
    o << "{\"type\":\"error\",\"ref\":\"" << (plain(ref) ? ref : "") << "\",\"reason\":\"" << reason
      << "\"}";
    sink_(o.str());
}

void Protocol::handle(std::string_view line) {
    const auto w = split(line);
    if (w.empty()) return;
    const std::string_view cmd = w[0];
    const std::string_view ref = w.size() > 1 ? w[1] : std::string_view();
    if (!plain(ref)) return reply_error("", "missing or bad ref");

    if (cmd == "NEW") {
        if (w.size() != 9)
            return reply_error(ref, "usage: NEW ref account symbol side type tif price qty");
        NewOrder n;
        if (!parse_int(w[2], n.account)) return reply_error(ref, "bad account");
        const auto sym = ex_.find_symbol(w[3]);
        if (!sym) return reply_error(ref, "unknown symbol");
        n.symbol = *sym;
        if (w[4] == "BUY")
            n.side = Side::Buy;
        else if (w[4] == "SELL")
            n.side = Side::Sell;
        else
            return reply_error(ref, "side must be BUY or SELL");
        if (w[5] == "LIMIT")
            n.type = OrderType::Limit;
        else if (w[5] == "MARKET")
            n.type = OrderType::Market;
        else
            return reply_error(ref, "type must be LIMIT or MARKET");
        if (w[6] == "GTC")
            n.tif = TimeInForce::GTC;
        else if (w[6] == "IOC")
            n.tif = TimeInForce::IOC;
        else if (w[6] == "FOK")
            n.tif = TimeInForce::FOK;
        else
            return reply_error(ref, "tif must be GTC, IOC or FOK");
        if (!parse_int(w[7], n.price)) return reply_error(ref, "bad price");
        if (!parse_int(w[8], n.qty)) return reply_error(ref, "bad quantity");
        if (n.type == OrderType::Market) n.price = 0;

        const Placed p = ex_.submit(n);
        std::ostringstream o;
        o << "{\"type\":\"ack\",\"ref\":\"" << ref << "\",\"id\":" << p.id << ",\"status\":\""
          << status_name(p.result.status) << "\",\"filled\":" << p.result.filled
          << ",\"remaining\":" << p.result.remaining;
        if (p.result.status == Status::Rejected)
            o << ",\"reason\":\"" << reason_name(p.result.reason) << '"';
        o << '}';
        sink_(o.str());
    } else if (cmd == "CANCEL") {
        Account account;
        OrderId id;
        if (w.size() != 4 || !parse_int(w[2], account) || !parse_int(w[3], id))
            return reply_error(ref, "usage: CANCEL ref account id");
        const bool ok = ex_.cancel(account, id);
        sink_(std::string("{\"type\":\"cancel\",\"ref\":\"") + std::string(ref) +
              "\",\"id\":" + std::to_string(id) + ",\"ok\":" + (ok ? "true" : "false") + "}");
    } else if (cmd == "SNAPSHOT") {
        if (w.size() < 3 || w.size() > 4)
            return reply_error(ref, "usage: SNAPSHOT ref symbol [levels]");
        const auto sym = ex_.find_symbol(w[2]);
        if (!sym) return reply_error(ref, "unknown symbol");
        std::size_t levels = SIZE_MAX;
        if (w.size() == 4 && !parse_int(w[3], levels)) return reply_error(ref, "bad levels");
        std::string json = to_json(ex_, ex_.snapshot(*sym, levels));
        json.insert(json.size() - 1, ",\"ref\":\"" + std::string(ref) + "\"");
        sink_(json);
    } else if (cmd == "LEDGER") {
        const Ledger& l = ex_.ledger();
        const std::string problem = l.check();
        std::ostringstream o;
        o << "{\"type\":\"ledger\",\"ref\":\"" << ref << "\",\"trades\":" << l.trade_count()
          << ",\"traded_cash\":" << l.traded_cash()
          << ",\"balanced\":" << (problem.empty() ? "true" : "false") << ",\"cash\":{";
        bool first = true;
        for (const auto& [a, c] : l.cash_balances()) {
            o << (first ? "" : ",") << '"' << a << "\":" << c;
            first = false;
        }
        o << "},\"positions\":{";
        first = true;
        for (const auto& [key, q] : l.positions()) {
            o << (first ? "" : ",") << "\"" << key.first << ':' << ex_.symbol_name(key.second)
              << "\":" << q;
            first = false;
        }
        o << "}}";
        sink_(o.str());
    } else if (cmd == "SYMBOLS") {
        std::ostringstream o;
        o << "{\"type\":\"symbols\",\"ref\":\"" << ref << "\",\"symbols\":[";
        for (std::size_t i = 0; i < ex_.symbol_count(); ++i) {
            o << (i ? "," : "") << '"' << ex_.symbol_name(static_cast<SymbolId>(i)) << '"';
        }
        o << "]}";
        sink_(o.str());
    } else {
        reply_error(ref, "unknown command");
    }
}

}  // namespace matchbook
