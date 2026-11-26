#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "matchbook/csv_replay.hpp"

namespace {

void usage() {
    std::cerr << "usage: matchbook [--book [N]] [orders.csv]\n"
                 "  Reads orders from the CSV file (or stdin) and prints trades.\n"
                 "  --book [N]  also print the top N levels per side at the end (default 5)\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    bool show_book = false;
    std::size_t levels = 5;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        }
        if (arg == "--book") {
            show_book = true;
            if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) {
                levels = static_cast<std::size_t>(std::atoi(argv[++i]));
            }
        } else if (path.empty()) {
            path = arg;
        } else {
            usage();
            return 2;
        }
    }

    std::ifstream file;
    if (!path.empty()) {
        file.open(path);
        if (!file) {
            std::cerr << "cannot open " << path << '\n';
            return 1;
        }
    }
    std::istream& in = path.empty() ? std::cin : file;

    matchbook::OrderBook book;
    const auto sum = matchbook::replay_csv(in, std::cout, std::cerr, book);
    if (show_book) matchbook::print_book(std::cout, book, levels);
    std::cerr << sum.commands << " commands, " << sum.trades << " trades, " << sum.rejects
              << " rejects, " << sum.bad_lines << " bad lines\n";
    return sum.bad_lines == 0 ? 0 : 1;
}
