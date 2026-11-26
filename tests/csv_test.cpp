#include <gtest/gtest.h>

#include <sstream>

#include "matchbook/csv_replay.hpp"

using namespace matchbook;

TEST(CsvParse, NewLimitOrder) {
    auto r = parse_line("NEW,7,BUY,LIMIT,IOC,101,12");
    ASSERT_TRUE(r.command) << r.error;
    EXPECT_EQ(r.command->kind, Command::Kind::New);
    EXPECT_EQ(r.command->order, limit_order(7, Side::Buy, 101, 12, TimeInForce::IOC));
}

TEST(CsvParse, MarketOrderAllowsEmptyPriceAndTif) {
    auto r = parse_line("new, 3 , sell , market , , , 5");
    ASSERT_TRUE(r.command) << r.error;
    EXPECT_EQ(r.command->order, market_order(3, Side::Sell, 5, TimeInForce::GTC));
}

TEST(CsvParse, CancelAndModify) {
    auto c = parse_line("CANCEL,9");
    ASSERT_TRUE(c.command);
    EXPECT_EQ(c.command->kind, Command::Kind::Cancel);
    EXPECT_EQ(c.command->id, 9u);
    auto m = parse_line("MODIFY,9,,,,105,3");
    ASSERT_TRUE(m.command);
    EXPECT_EQ(m.command->kind, Command::Kind::Modify);
    EXPECT_EQ(m.command->price, 105);
    EXPECT_EQ(m.command->qty, 3);
}

TEST(CsvParse, SkipsHeaderBlankAndComments) {
    for (const char* line : {"action,id,side,type,tif,price,qty", "", "   ", "# note"}) {
        auto r = parse_line(line);
        EXPECT_FALSE(r.command) << line;
        EXPECT_TRUE(r.error.empty()) << line;
    }
}

TEST(CsvParse, ReportsBadLines) {
    for (const char* line :
         {"NEW,1,UP,LIMIT,GTC,100,5", "NEW,x,BUY,LIMIT,GTC,100,5", "NEW,1,BUY,LIMIT,GTC,10.5,5",
          "NEW,1,BUY,LIMIT,GTC,100,", "NEW,1,BUY,STOP,GTC,100,5", "NEW,1,BUY,LIMIT,DAY,100,5",
          "CANCEL", "MODIFY,1,,,,100", "HOLD,1"}) {
        auto r = parse_line(line);
        EXPECT_FALSE(r.command) << line;
        EXPECT_FALSE(r.error.empty()) << line;
    }
}

TEST(CsvReplay, PrintsTradesAndRejects) {
    std::istringstream in(
        "action,id,side,type,tif,price,qty\n"
        "NEW,1,SELL,LIMIT,GTC,100,5\n"
        "NEW,2,BUY,LIMIT,GTC,101,8\n"
        "CANCEL,77\n"
        "NEW,2,BUY,LIMIT,GTC,90,1\n");
    std::ostringstream out, err;
    OrderBook book;
    auto sum = replay_csv(in, out, err, book);
    EXPECT_EQ(out.str(),
              "TRADE,2,1,BUY,100,5\n"
              "REJECT,77,UnknownId\n"
              "REJECT,2,DuplicateId\n");
    EXPECT_EQ(err.str(), "");
    EXPECT_EQ(sum.commands, 4u);
    EXPECT_EQ(sum.trades, 1u);
    EXPECT_EQ(sum.rejects, 2u);
    EXPECT_EQ(book.best_bid(), 101);
}

TEST(CsvReplay, BadLinesGoToStderrWithLineNumbers) {
    std::istringstream in("NEW,1,BUY,LIMIT,GTC,100,5\nnonsense\nCANCEL,1\n");
    std::ostringstream out, err;
    OrderBook book;
    auto sum = replay_csv(in, out, err, book);
    EXPECT_EQ(err.str(), "line 2: action must be NEW, CANCEL or MODIFY\n");
    EXPECT_EQ(sum.bad_lines, 1u);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(CsvReplay, PrintBook) {
    OrderBook book;
    std::vector<Trade> t;
    book.submit(limit_order(1, Side::Buy, 99, 4), t);
    book.submit(limit_order(2, Side::Sell, 101, 3), t);
    book.submit(limit_order(3, Side::Sell, 102, 1), t);
    std::ostringstream out;
    print_book(out, book, 5);
    EXPECT_EQ(out.str(),
              "BOOK\n"
              "  ASK 102 x 1 (1 orders)\n"
              "  ASK 101 x 3 (1 orders)\n"
              "  ---\n"
              "  BID 99 x 4 (1 orders)\n");
}
