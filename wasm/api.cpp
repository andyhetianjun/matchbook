// The exchange compiled to WebAssembly for the browser demo (see web/index.html).
// Plain C functions, so the page can call them with no extra tooling:
//
//   mb_init(seed)         start a fresh market with three example products
//   mb_command(line)      one request line (see include/matchbook/wire.hpp); returns the
//                         JSON reply lines (feed messages first, then the reply), one per line
//   mb_traffic(ops)       run that many seeded random operations; returns the trade count
//   mb_state(symbol)      a JSON summary of one product for drawing the page

#include <emscripten.h>

#include <deque>
#include <memory>
#include <sstream>
#include <string>

#include "matchbook/traffic.hpp"
#include "matchbook/wire.hpp"

using namespace matchbook;

namespace {

constexpr Account kYou = 0;   // the person at the keyboard
constexpr Account kBots = 8;  // accounts 1..8 send the random traffic
constexpr std::size_t kLedgerEntries = 20000;
constexpr std::size_t kRecentMessages = 600;
const char* const kSymbols[] = {"WHEAT", "COPPER", "SALMON"};

struct Market {
    Exchange ex{kLedgerEntries};
    std::string out;  // reply lines for the command being handled
    std::unique_ptr<Protocol> proto;
    std::unique_ptr<TrafficGen> traffic;
    std::deque<FeedMessage> recent;  // for the trades tape and feed panel
    std::string result;              // buffer handed back to JavaScript

    explicit Market(std::uint64_t seed) {
        for (const char* s : kSymbols) ex.add_symbol(s);
        ex.subscribe([this](const FeedMessage& m) {
            recent.push_back(m);
            if (recent.size() > kRecentMessages) recent.pop_front();
        });
        proto = std::make_unique<Protocol>(ex, [this](const std::string& l) {
            out += l;
            out += '\n';
        });
        TrafficGen::Config cfg;
        cfg.seed = seed;
        cfg.first_account = 1;
        cfg.accounts = kBots;
        cfg.start_mid = 100;
        traffic = std::make_unique<TrafficGen>(ex, cfg);
    }
};

std::unique_ptr<Market> g_market;

void levels(std::ostringstream& o, const std::vector<DepthLevel>& v) {
    o << '[';
    for (std::size_t i = 0; i < v.size(); ++i) {
        o << (i ? "," : "") << '[' << v[i].price << ',' << v[i].qty << ',' << v[i].orders << ']';
    }
    o << ']';
}

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE void mb_init(unsigned seed) {
    g_market = std::make_unique<Market>(seed);
    g_market->proto->set_feed_output(false);
    g_market->traffic->run(600);  // so the books are not empty when the page opens
    g_market->proto->set_feed_output(true);
}

EMSCRIPTEN_KEEPALIVE const char* mb_command(const char* line) {
    Market& m = *g_market;
    m.out.clear();
    m.proto->handle(line);
    m.result = m.out;
    return m.result.c_str();
}

EMSCRIPTEN_KEEPALIVE unsigned mb_traffic(unsigned ops) {
    Market& m = *g_market;
    m.proto->set_feed_output(false);
    const auto trades = m.traffic->run(ops);
    m.proto->set_feed_output(true);
    return static_cast<unsigned>(trades);
}

EMSCRIPTEN_KEEPALIVE const char* mb_state(unsigned symbol_index) {
    Market& m = *g_market;
    const Exchange& ex = m.ex;
    const auto sym = static_cast<SymbolId>(symbol_index);
    std::ostringstream o;
    o << "{\"symbol\":\"" << ex.symbol_name(sym) << "\",\"seq\":" << ex.seq(sym) << ",\"bids\":";
    levels(o, ex.book(sym).depth(Side::Buy, 10));
    o << ",\"asks\":";
    levels(o, ex.book(sym).depth(Side::Sell, 10));
    o << ",\"orders\":" << ex.book(sym).order_count();

    // The newest trades, newest first.
    o << ",\"trades\":[";
    int shown = 0;
    for (auto it = m.recent.rbegin(); it != m.recent.rend() && shown < 14; ++it) {
        if (it->symbol != sym) continue;
        if (const auto* t = std::get_if<TradePrint>(&it->body)) {
            o << (shown ? "," : "") << "[" << it->seq << ',' << t->price << ',' << t->qty << ','
              << (t->taker_side == Side::Buy ? "\"buy\"" : "\"sell\"") << ']';
            ++shown;
        }
    }
    o << "],\"feed\":[";
    shown = 0;
    for (auto it = m.recent.rbegin(); it != m.recent.rend() && shown < 12; ++it) {
        if (it->symbol != sym) continue;
        o << (shown ? "," : "") << to_json(ex, *it);
        ++shown;
    }

    // Your open orders and the ledger.
    o << "],\"mine\":[";
    bool first = true;
    for (const auto& oo : ex.open_orders(kYou)) {
        if (oo.symbol != sym) continue;
        o << (first ? "" : ",") << "[" << oo.id << ",\"" << (oo.side == Side::Buy ? "buy" : "sell")
          << "\"," << oo.price << ',' << oo.qty << ']';
        first = false;
    }
    const Ledger& l = ex.ledger();
    o << "],\"ledger\":{\"trades\":" << l.trade_count()
      << ",\"balanced\":" << (l.check().empty() ? "true" : "false")
      << ",\"traded_cash\":" << l.traded_cash() << ",\"your_cash\":" << l.cash(kYou)
      << ",\"your_position\":" << l.position(kYou, sym) << ",\"entries\":" << l.entries().size()
      << "}}";
    m.result = o.str();
    return m.result.c_str();
}

}  // extern "C"
