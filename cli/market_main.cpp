// matchbook_market: reads request lines from stdin (see matchbook/wire.hpp) and writes
// JSON lines to stdout. The gateway runs this as a child process.

#include <iostream>
#include <string>

#include "matchbook/wire.hpp"

int main(int argc, char** argv) {
    matchbook::Exchange ex;
    // Symbols come from the command line, or a small default set.
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) ex.add_symbol(argv[i]);
    } else {
        for (const char* s : {"AAPL", "MSFT", "BTC"}) ex.add_symbol(s);
    }
    matchbook::Protocol proto(ex, [](const std::string& line) { std::cout << line << '\n'; });
    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        proto.handle(line);
        std::cout.flush();
    }
    return 0;
}
