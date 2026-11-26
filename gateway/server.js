// A small WebSocket gateway in front of the matchbook_market process.
//
// The C++ exchange reads one request per line on stdin and writes one JSON line per
// reply or feed message on stdout (see include/matchbook/wire.hpp). This file does
// three jobs: turn a client's JSON message into a request line, send each reply
// back to the client that asked, and pass the market data feed on to the clients
// that subscribed to that symbol.
//
// Each connection is given its own account number. Clients cannot choose it, so
// one client cannot cancel another's orders.

import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';
import { WebSocketServer } from 'ws';

const SYMBOL = /^[A-Z0-9]{1,12}$/;
const MAX_BUFFERED = 1 << 20; // drop a client that falls this far behind

const isInt = (v) => Number.isSafeInteger(v) && v >= 0;

// Turns a client message into a request line (without the ref), or an error.
function toRequest(msg, account) {
  const fail = (error) => ({ error });
  switch (msg?.op) {
    case 'new': {
      const side = String(msg.side).toUpperCase();
      const type = String(msg.type ?? 'limit').toUpperCase();
      const tif = String(msg.tif ?? 'gtc').toUpperCase();
      if (!SYMBOL.test(msg.symbol)) return fail('bad symbol');
      if (!['BUY', 'SELL'].includes(side)) return fail('side must be buy or sell');
      if (!['LIMIT', 'MARKET'].includes(type)) return fail('type must be limit or market');
      if (!['GTC', 'IOC', 'FOK'].includes(tif)) return fail('tif must be gtc, ioc or fok');
      if (!isInt(msg.qty)) return fail('qty must be a whole number');
      if (type === 'LIMIT' && !isInt(msg.price)) return fail('price must be a whole number of ticks');
      const price = type === 'LIMIT' ? msg.price : 0;
      return { line: `NEW ${account} ${msg.symbol} ${side} ${type} ${tif} ${price} ${msg.qty}` };
    }
    case 'cancel':
      if (!isInt(msg.id)) return fail('id must be a whole number');
      return { line: `CANCEL ${account} ${msg.id}` };
    case 'snapshot':
    case 'subscribe':
      if (!SYMBOL.test(msg.symbol)) return fail('bad symbol');
      return { line: `SNAPSHOT ${msg.symbol}` };
    case 'ledger':
      return { line: 'LEDGER' };
    default:
      return fail('unknown op');
  }
}

export async function startGateway({ bin, symbols = [], port = 8080, host = '127.0.0.1' }) {
  const child = spawn(bin, symbols, { stdio: ['pipe', 'pipe', 'inherit'] });
  const clients = new Map(); // ws -> { account, subs: Set<symbol> }
  const pending = new Map(); // our ref -> { ws, ref, subscribe }
  let nextRef = 1;
  let nextAccount = 1;

  const send = (ws, obj) => {
    if (ws.readyState !== ws.OPEN) return;
    if (ws.bufferedAmount > MAX_BUFFERED) return ws.terminate();
    ws.send(typeof obj === 'string' ? obj : JSON.stringify(obj));
  };

  // Requests the exchange itself makes before we open the port (symbol list).
  let onHello;
  const hello = new Promise((resolve, reject) => {
    onHello = resolve;
    child.once('error', reject); // for example, the binary does not exist
  });

  createInterface({ input: child.stdout }).on('line', (line) => {
    let msg;
    try {
      msg = JSON.parse(line);
    } catch {
      return; // not ours to forward
    }
    if (msg.ref === 'boot') return onHello(msg.symbols);
    if (msg.ref !== undefined) {
      // A reply to one request: goes to the client that asked.
      const p = pending.get(msg.ref);
      if (!p) return;
      pending.delete(msg.ref);
      const c = clients.get(p.ws);
      if (!c) return;
      msg.ref = p.ref;
      if (p.subscribe && msg.type === 'snapshot') c.subs.add(msg.symbol);
      send(p.ws, msg);
    } else {
      // Feed message: everyone subscribed to this symbol. Because the exchange writes
      // lines in order, a client added when its snapshot goes by sees every later message.
      const text = line;
      for (const [ws, c] of clients) if (c.subs.has(msg.symbol)) send(ws, text);
    }
  });

  child.stdin.write('SYMBOLS boot\n');
  const known = new Set(await hello);

  const wss = new WebSocketServer({ port, host, maxPayload: 4096 });
  await new Promise((resolve) => wss.once('listening', resolve));

  wss.on('connection', (ws) => {
    const state = { account: nextAccount++, subs: new Set() };
    clients.set(ws, state);
    send(ws, { type: 'hello', account: state.account, symbols: [...known] });

    ws.on('message', (data) => {
      let msg;
      try {
        msg = JSON.parse(data.toString());
      } catch {
        return send(ws, { type: 'error', reason: 'not JSON' });
      }
      const clientRef = typeof msg?.ref === 'string' || typeof msg?.ref === 'number' ? msg.ref : null;
      if (msg?.op === 'unsubscribe') {
        state.subs.delete(msg.symbol);
        return send(ws, { type: 'unsubscribed', symbol: msg.symbol, ref: clientRef });
      }
      const { line, error } = toRequest(msg, state.account);
      if (error) return send(ws, { type: 'error', reason: error, ref: clientRef });
      if (/^(NEW|SNAPSHOT)/.test(line) && !known.has(msg.symbol)) {
        return send(ws, { type: 'error', reason: 'unknown symbol', ref: clientRef });
      }
      const ref = `g${nextRef++}`;
      pending.set(ref, { ws, ref: clientRef, subscribe: msg.op === 'subscribe' });
      const [cmd, ...rest] = line.split(' ');
      child.stdin.write([cmd, ref, ...rest].join(' ') + '\n');
    });
    ws.on('close', () => {
      clients.delete(ws);
      for (const [ref, p] of pending) if (p.ws === ws) pending.delete(ref);
    });
    ws.on('error', () => ws.terminate());
  });

  return {
    port: wss.address().port,
    symbols: [...known],
    async close() {
      for (const ws of wss.clients) ws.terminate();
      await new Promise((resolve) => wss.close(resolve));
      child.stdin.end();
      if (child.exitCode === null) await new Promise((resolve) => child.once('exit', resolve));
    },
  };
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const bin = process.env.MATCHBOOK_MARKET ?? fileURLToPath(new URL('../build/release/matchbook_market', import.meta.url));
  const port = Number(process.env.PORT ?? 8080);
  const gw = await startGateway({ bin, symbols: ['AAPL', 'MSFT', 'BTC'], port });
  console.log(`gateway listening on ws://127.0.0.1:${gw.port} for ${gw.symbols.join(', ')}`);
  const stop = () => gw.close().then(() => process.exit(0));
  process.on('SIGINT', stop);
  process.on('SIGTERM', stop);
}
