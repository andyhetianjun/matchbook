# WebSocket gateway

A small server that lets other programs send orders to the exchange and receive the market data feed over a WebSocket.

## Why I added this

The exchange so far could only be driven from C++ or a text file. I wanted to see what it takes to put it on a network,
and in particular whether the snapshot-then-incremental feed from `docs/multi-instrument-market.md` really lets a client
connect at any moment and end up with the right book.

## What it does

You run one command, and then any WebSocket client can connect. Each connection gets its own account number. A client can:

| Send | What happens |
|---|---|
| `{"op":"new","symbol":"AAPL","side":"buy","type":"limit","tif":"gtc","price":101,"qty":10,"ref":"a1"}` | places an order; you get an `ack` with the same `ref` |
| `{"op":"cancel","id":7}` | cancels your own order |
| `{"op":"subscribe","symbol":"AAPL"}` | you get a `snapshot`, then every feed message for that symbol after it |
| `{"op":"unsubscribe","symbol":"AAPL"}` | stops the feed for that symbol |
| `{"op":"ledger"}` | the ledger totals and whether they balance |

A client picks its own `ref` for each request and the reply carries it back. Prices are whole numbers of ticks.

## How to try it

```sh
cmake --preset release && cmake --build --preset release -j     # builds build/release/matchbook_market
cd gateway && npm ci
node server.js                 # listens on ws://127.0.0.1:8080 (PORT=9000 to change)
```

Then from another terminal, using the `ws` package that was just installed:

```sh
node -e "
const ws = new (require('ws'))('ws://127.0.0.1:8080');
ws.on('open', () => { ws.send(JSON.stringify({op:'subscribe', symbol:'AAPL'}));
  ws.send(JSON.stringify({op:'new', symbol:'AAPL', side:'buy', price:100, qty:5, ref:'x'})); });
ws.on('message', (m) => console.log(m.toString()));
setTimeout(() => process.exit(), 500);"
```

## How it works

The gateway is a thin Node program (`gateway/server.js`, about 150 lines) around the C++ `matchbook_market` process.
It starts that process as a child. It writes one request line per client message to the child's input, and reads one JSON
line per reply or feed message from its output. I chose this over linking a WebSocket library into the C++ code because
the matching code stays free of network code and I could test the whole thing with ordinary tools.

```
 client ──JSON──> gateway ──"NEW g7 3 AAPL BUY LIMIT GTC 101 10"──> matchbook_market
 client <──JSON── gateway <── ack / level / top / trade / snapshot ── matchbook_market
```

- **Checking input.** The gateway checks every field (symbols are letters and digits, prices and quantities are whole
  numbers) before building the request line, so a client cannot inject extra commands. The exchange process checks again.
- **Account numbers** are handed out by the gateway, one per connection, and a client cannot choose it. That is why one
  client cannot cancel another's orders. There is no login: this is a demo of the plumbing, not a security design.
- **Joining late without gaps.** The exchange process writes its output in order. When a client subscribes, the gateway asks for
  a snapshot. When the snapshot reply goes by, the gateway adds the client to that symbol's subscribers and sends it the
  snapshot. Every later feed line for the symbol is then sent to that client, and none can fall in between because
  everything comes down one ordered pipe. The first message after the snapshot has sequence number snapshot.seq + 1.
- **Slow clients** that let more than 1 MB queue up are disconnected rather than allowed to use memory.

## Integration test

`gateway/test/gateway.test.js` starts the real `matchbook_market` binary and the gateway on a random port, connects real
WebSocket clients, and checks that:

- a new client is greeted with its account number and the symbols,
- orders get the right ack, and a subscriber sees the level update, then the trade, with consecutive sequence numbers
  and nothing from products it did not subscribe to,
- a client that subscribes late gets a snapshot with the right depth and its next message continues from the snapshot's
  sequence number,
- only the owner can cancel an order,
- bad input (not JSON, a fractional price, an unknown symbol, an injection attempt in a field) gets an error and the
  gateway keeps working.

```sh
cmake --preset release && cmake --build --preset release -j
cd gateway && npm ci && npm test
```

Result on my machine: 5 passing tests in about 0.1 seconds. CI runs the same steps in the `gateway` job.

## Limits

- Orders that a client leaves resting stay on the book after it disconnects.
- It has no authentication, rate limits or TLS.
- It forwards each feed line as it arrives (no batching), and I have not measured how many clients or messages per
  second it handles.
