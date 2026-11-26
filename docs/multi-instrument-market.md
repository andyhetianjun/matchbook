# Many products, a market data feed, and a settlement ledger

The first version of matchbook had one order book. A real market has many products at once, tells everyone what is
happening, and has to settle who owes what after each trade. This page covers the three pieces I added for that.

## Why I added this

I wanted to see what sits around a matching engine. A book on its own answers "who trades with whom". It does not
answer "how does someone who connects late find out the current book?" or "how do I know the money adds up?". Both
sounded easy and both turned out to have a few traps, so I built them and wrote tests that try to break them.

## What it does, in plain words

- **Many books.** An `Exchange` holds one order book per product (symbol). Orders for one product never touch
  another product's book. Every order gets an id that is unique across the whole exchange, and the exchange remembers
  which account placed each resting order, so only that account can cancel it.
- **A market data feed.** Every change is published as a small numbered message. There are three kinds:
  - `trade`: a trade happened at this price and quantity.
  - `level`: the total quantity resting at one price changed (0 means that price is empty now).
  - `top`: the best bid or best ask changed.
- **Snapshot plus incremental updates.** Each product counts its messages 1, 2, 3 and so on. A client that joins late asks
  for a snapshot (the whole book, tagged with the latest sequence number N), then applies every message numbered N+1
  and up. If it ever sees a number skipped, it knows it missed something and asks for a new snapshot.
- **A settlement ledger.** Every trade is written down as who owes what: the buyer owes the seller `price x quantity`
  in cash, and the seller owes the buyer the quantity of the product. The ledger keeps a running cash balance per account
  and a running position per account and product, and `check()` proves that nothing was created or lost.

## How to try it

```sh
cmake --preset release && cmake --build --preset release -j
printf 'NEW a 1 AAPL SELL LIMIT GTC 101 5\nNEW b 2 AAPL BUY LIMIT GTC 102 3\nSNAPSHOT s AAPL 3\nLEDGER l\n' \
  | ./build/release/matchbook_market
```

That sends two orders from accounts 1 and 2, asks for a snapshot, and asks for the ledger report. The output is one JSON
line per message (shortened here):

```
{"type":"level","symbol":"AAPL","seq":1,"side":"sell","price":101,"qty":5}
{"type":"top","symbol":"AAPL","seq":2,"bid":null,"ask":[101,5]}
{"type":"ack","ref":"a","id":1,"status":"resting","filled":0,"remaining":5}
{"type":"trade","symbol":"AAPL","seq":3,"taker":2,"maker":1,"side":"buy","price":101,"qty":3}
{"type":"level","symbol":"AAPL","seq":4,"side":"sell","price":101,"qty":2}
{"type":"top","symbol":"AAPL","seq":5,"bid":null,"ask":[101,2]}
{"type":"ack","ref":"b","id":2,"status":"filled","filled":3,"remaining":0}
{"type":"snapshot","symbol":"AAPL","seq":5,"bids":[],"asks":[[101,2]],"ref":"s"}
{"type":"ledger","ref":"l","trades":1,"traded_cash":303,"balanced":true,"cash":{"1":303,"2":-303},"positions":{"1:AAPL":-3,"2:AAPL":3}}
```

Note that the trade is at 101, the price of the order that was already resting, not the buyer's 102. The request format
is documented in `include/matchbook/wire.hpp`.

## How it works

```
 orders ──> Exchange ──> OrderBook per symbol ──> trades
              │                                      │
              │   level changes (looked up after     ├──> Ledger  (who owes what)
              │   each operation)                    │
              └──> numbered feed messages <──────────┘
                        │
                        ├──> listeners (gateway, browser page, tests)
                        └──> snapshot(symbol) = book + latest sequence number
```

- **Finding what changed.** The order book itself is unchanged apart from one new read-only method,
  `level_at(side, price)`. After each operation the exchange knows which prices could have changed: the prices of
  the trades, the price where the order rested, and the price a cancelled or modified order left. It looks up each of
  those levels once and publishes the new total. That costs one tree lookup per changed price instead of comparing whole
  depth snapshots.
- **Message order.** For one operation: trades first (in the order they happened), then the level updates, then `top` if
  the best prices changed. A mid-operation state is never published, since the exchange is single threaded.
- **Snapshots are exact.** Because everything runs on one thread, a snapshot taken between two operations belongs to
  exactly one sequence number. No message can fall in between.
- **Ownership.** The exchange keeps a table from order id to account for resting orders only, and removes an entry the
  moment an order is filled or cancelled, so the table does not grow with history.
- **The ledger keeps two views.** Balances are updated as trades arrive. `check()` also rebuilds the balances from the
  stored entries and compares them. The demo and the benchmark cap how many entries are stored (the balances are always
  complete), and when entries have been dropped `check()` falls back to the sum checks only.

## Tests

`ctest --preset release` runs 78 tests. 27 of them are new:

- `tests/exchange_test.cpp`: books are independent, ids are unique across products, sequence numbers have no gaps, the
  exact messages for a partial fill, `top` is only sent when it changes, only the owner can cancel, a late joiner
  rebuilds the book, the mirror detects a gap, self-trades net to zero, and the ledger records the right buyer and seller.
- `tests/market_random_test.cpp`: the randomized part. Each stream mixes limit, market, IOC and FOK orders, cancels
  (half of them from the wrong account, which must always fail) and modifies across 1 to 4 products and 1 to 7 accounts.
  After every operation the feed must have no gaps. Every 97 steps (and at the end) the test checks that:
  1. every book passes its own consistency check (`validate()`),
  2. a client that rebuilt its book from the whole feed equals the real book,
  3. a client that joined half way through from a snapshot also equals the real book,
  4. the ledger's `check()` passes.

  At the end the test also computes every account's cash and positions from the trade messages and the test's own
  record of who placed which order, and requires the ledger to agree exactly. Total cash must be 0 and the positions in
  each product must sum to 0. It runs 24 seeded streams of 4,000 operations, a stream where every order comes from one account, and a
  40,000 operation stream with a wide price range.
- `tests/wire_test.cpp`: the text protocol, including malformed requests.

To check the tests can fail, I temporarily made the ledger credit the seller one extra unit on trades of quantity 7.
The randomized tests, the ledger test and the traffic test all failed; I reverted it.

```sh
ctest --preset release
cmake --preset sanitize && cmake --build --preset sanitize -j && ctest --preset sanitize   # same 78 tests under ASan and UBSan
```

## Measured speed

`bench/market_bench.cpp` pushes the same seeded random orders the browser demo uses through the whole exchange
(3 books, the feed, and the ledger), with a listener counting the feed messages. It does not include JSON encoding.

```sh
./build/release/matchbook_market_bench --ops 2000000 --symbols 3 --seed 42 --runs 5
```

On an Apple M5 Pro, Release build, with other work running on the machine (load average 6 to 8), the median was about
4.4 million operations per second, with 1,346,186 trades and 4,212,021 feed messages per 2,000,000 operations, and the
ledger balanced after every run. That is roughly a third of the single book's 13.3 million in the README, which is what
I would expect, since the exchange also publishes the feed and records the ledger. I have not profiled where the rest goes.

## Limits

- It is single threaded, like the book.
- Accounts have no limits: nothing stops a balance going negative. The ledger records what happened and checks it adds
  up. It does not decide whether a trade should have been allowed.
- The feed is an in-process callback. The gateway (see `docs/gateway.md`) is what puts it on the network, and nothing
  re-sends a message that a slow client missed. The client just asks for a new snapshot.
