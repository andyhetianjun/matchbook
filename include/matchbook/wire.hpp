#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "matchbook/exchange.hpp"

namespace matchbook {

// JSON for the feed and the other things a client sees. One JSON object per line.
std::string to_json(const Exchange& ex, const FeedMessage& msg);
std::string to_json(const Exchange& ex, const BookSnapshot& snap);

// A line-based text protocol on top of an Exchange, used by the gateway and by the
// in-browser demo. A request is one line of words, and every reply is one JSON line.
//
//   NEW <ref> <account> <symbol> <BUY|SELL> <LIMIT|MARKET> <GTC|IOC|FOK> <price> <qty>
//   CANCEL <ref> <account> <order id>
//   SNAPSHOT <ref> <symbol> [levels]
//   LEDGER <ref>
//   SYMBOLS <ref>
//
// `ref` is any token without spaces; the reply to a request carries the same ref.
// Feed messages are also written to the sink as they are published.
class Protocol {
public:
    using Sink = std::function<void(const std::string& line)>;

    Protocol(Exchange& ex, Sink sink);
    void handle(std::string_view line);

private:
    void reply_error(std::string_view ref, std::string_view reason);
    Exchange& ex_;
    Sink sink_;
};

}  // namespace matchbook
