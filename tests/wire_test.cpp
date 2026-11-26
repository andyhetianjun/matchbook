#include "matchbook/wire.hpp"

#include <gtest/gtest.h>

using namespace matchbook;

namespace {
struct Fixture {
    Exchange ex;
    std::vector<std::string> out;
    Protocol proto;
    Fixture() : proto(ex, [this](const std::string& l) { out.push_back(l); }) {
        ex.add_symbol("AAPL");
        ex.add_symbol("MSFT");
    }
    std::string last() const { return out.back(); }
};
bool has(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}
}  // namespace

TEST(Wire, NewOrderAcksAndPublishes) {
    Fixture f;
    f.proto.handle("NEW r1 1 AAPL BUY LIMIT GTC 100 10");
    ASSERT_EQ(f.out.size(), 3u);  // level, top, ack
    EXPECT_EQ(f.out[0],
              R"({"type":"level","symbol":"AAPL","seq":1,"side":"buy","price":100,"qty":10})");
    EXPECT_EQ(f.out[1], R"({"type":"top","symbol":"AAPL","seq":2,"bid":[100,10],"ask":null})");
    EXPECT_EQ(f.last(),
              R"({"type":"ack","ref":"r1","id":1,"status":"resting","filled":0,"remaining":10})");
}

TEST(Wire, TradeLine) {
    Fixture f;
    f.proto.handle("NEW a 1 AAPL BUY LIMIT GTC 100 10");
    f.out.clear();
    f.proto.handle("NEW b 2 AAPL SELL MARKET IOC 0 4");
    EXPECT_EQ(
        f.out[0],
        R"({"type":"trade","symbol":"AAPL","seq":3,"taker":2,"maker":1,"side":"sell","price":100,"qty":4})");
    EXPECT_TRUE(has(f.last(), R"("status":"filled")"));
}

TEST(Wire, CancelNeedsTheOwner) {
    Fixture f;
    f.proto.handle("NEW a 1 AAPL BUY LIMIT GTC 100 10");
    f.proto.handle("CANCEL c1 2 1");
    EXPECT_EQ(f.last(), R"({"type":"cancel","ref":"c1","id":1,"ok":false})");
    f.proto.handle("CANCEL c2 1 1");
    EXPECT_EQ(f.last(), R"({"type":"cancel","ref":"c2","id":1,"ok":true})");
}

TEST(Wire, SnapshotCarriesTheSeqAndRef) {
    Fixture f;
    f.proto.handle("NEW a 1 MSFT SELL LIMIT GTC 205 3");
    f.proto.handle("SNAPSHOT s1 MSFT 5");
    EXPECT_EQ(
        f.last(),
        R"({"type":"snapshot","symbol":"MSFT","seq":2,"bids":[],"asks":[[205,3]],"ref":"s1"})");
}

TEST(Wire, BadInputGetsAnErrorNotACrash) {
    Fixture f;
    for (const char* bad :
         {"NEW r1", "NEW r1 x AAPL BUY LIMIT GTC 1 1", "NEW r1 1 NOPE BUY LIMIT GTC 1 1",
          "NEW r1 1 AAPL HOLD LIMIT GTC 1 1", "NEW r1 1 AAPL BUY LIMIT GTC 1 -", "FOO r1",
          "SNAPSHOT r1 NOPE", "CANCEL r1 1", "NEW"}) {
        f.out.clear();
        f.proto.handle(bad);
        ASSERT_EQ(f.out.size(), 1u) << bad;
        EXPECT_TRUE(has(f.last(), "\"type\":\"error\"")) << bad;
    }
    f.out.clear();
    f.proto.handle("");
    EXPECT_TRUE(f.out.empty());
}

TEST(Wire, RejectedOrderIsReportedInTheAck) {
    Fixture f;
    f.proto.handle("NEW r 1 AAPL BUY LIMIT GTC 100 0");
    EXPECT_TRUE(has(f.last(), R"("status":"rejected")"));
    EXPECT_TRUE(has(f.last(), R"("reason":"bad quantity")"));
}

TEST(Wire, LedgerReport) {
    Fixture f;
    f.proto.handle("NEW a 1 AAPL SELL LIMIT GTC 100 5");
    f.proto.handle("NEW b 2 AAPL BUY LIMIT GTC 100 5");
    f.proto.handle("LEDGER l");
    EXPECT_TRUE(has(f.last(), R"("balanced":true)"));
    EXPECT_TRUE(has(f.last(), R"("cash":{"1":500,"2":-500})"));
    EXPECT_TRUE(has(f.last(), R"("1:AAPL":-5)"));
}
