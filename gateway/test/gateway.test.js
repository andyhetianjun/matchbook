// Integration test: starts the real matchbook_market process and the gateway, connects
// WebSocket clients, and checks orders, acks, the feed and late joining.
//
// Needs the C++ binary: cmake --preset release && cmake --build --preset release
// (or set MATCHBOOK_MARKET to its path).

import assert from 'node:assert/strict';
import { after, before, describe, it } from 'node:test';
import { fileURLToPath } from 'node:url';
import { WebSocket } from 'ws';
import { startGateway } from '../server.js';

const bin = process.env.MATCHBOOK_MARKET ?? fileURLToPath(new URL('../../build/release/matchbook_market', import.meta.url));

// A client that records everything it receives and lets a test wait for a message.
function connect(port) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}`);
  const seen = [];
  const waiters = [];
  ws.on('message', (data) => {
    const m = JSON.parse(data.toString());
    seen.push(m);
    for (const w of [...waiters]) {
      if (w.match(m)) {
        waiters.splice(waiters.indexOf(w), 1);
        w.resolve(m);
      }
    }
  });
  const next = (match) =>
    new Promise((resolve, reject) => {
      const found = seen.find((m) => match(m) && !m._taken);
      if (found) return resolve(found);
      const t = setTimeout(() => reject(new Error('timed out waiting for a message')), 3000);
      waiters.push({ match, resolve: (m) => (clearTimeout(t), resolve(m)) });
    });
  const opened = new Promise((resolve) => ws.once('open', resolve));
  return {
    ws,
    seen,
    next,
    ready: async () => (await opened, next((m) => m.type === 'hello')),
    send: (obj) => ws.send(JSON.stringify(obj)),
    close: () => new Promise((resolve) => (ws.once('close', resolve), ws.close())),
  };
}

describe('gateway', () => {
  let gw;
  before(async () => {
    gw = await startGateway({ bin, symbols: ['AAPL', 'MSFT'], port: 0 });
  });
  after(async () => {
    await gw.close();
  });

  it('greets a client with its account and the symbols', async () => {
    const a = connect(gw.port);
    const hello = await a.ready();
    assert.deepEqual(hello.symbols.sort(), ['AAPL', 'MSFT']);
    assert.ok(Number.isInteger(hello.account));
    await a.close();
  });

  it('takes orders, answers each one, and streams the feed to subscribers', async () => {
    const a = connect(gw.port);
    const b = connect(gw.port);
    await a.ready();
    await b.ready();

    a.send({ op: 'subscribe', symbol: 'AAPL', ref: 's' });
    const snap = await a.next((m) => m.type === 'snapshot' && m.ref === 's');
    assert.deepEqual(snap.bids, []);
    // MSFT is not subscribed, so nothing from MSFT should reach a.
    b.send({ op: 'new', symbol: 'MSFT', side: 'sell', type: 'limit', price: 300, qty: 1, ref: 'x' });
    await b.next((m) => m.type === 'ack' && m.ref === 'x');

    b.send({ op: 'new', symbol: 'AAPL', side: 'sell', type: 'limit', price: 101, qty: 10, ref: 'r1' });
    const ack = await b.next((m) => m.type === 'ack' && m.ref === 'r1');
    assert.equal(ack.status, 'resting');
    const lvl = await a.next((m) => m.type === 'level' && m.price === 101);
    assert.equal(lvl.qty, 10);
    assert.equal(lvl.seq, snap.seq + 1);

    a.send({ op: 'new', symbol: 'AAPL', side: 'buy', type: 'limit', price: 102, qty: 4, ref: 'r2' });
    const ack2 = await a.next((m) => m.type === 'ack' && m.ref === 'r2');
    assert.equal(ack2.status, 'filled');
    const trade = await a.next((m) => m.type === 'trade');
    assert.equal(trade.price, 101);
    assert.equal(trade.qty, 4);
    assert.equal(trade.maker, ack.id);
    assert.equal(trade.taker, ack2.id);

    // Whatever a received for AAPL has consecutive sequence numbers, and none from MSFT.
    const feed = a.seen.filter((m) => ['level', 'top', 'trade'].includes(m.type));
    assert.ok(feed.every((m) => m.symbol === 'AAPL'));
    feed.reduce((prev, m) => (assert.equal(m.seq, prev + 1), m.seq), snap.seq);

    // The other client did not subscribe, so it gets acks only.
    assert.ok(!b.seen.some((m) => m.type === 'trade'));
    await a.close();
    await b.close();
  });

  it('lets a client that joins late rebuild the book from a snapshot', async () => {
    const maker = connect(gw.port);
    await maker.ready();
    for (const [i, price] of [90, 91, 92].entries()) {
      maker.send({ op: 'new', symbol: 'MSFT', side: 'buy', type: 'limit', price, qty: 5 + i, ref: `b${i}` });
      await maker.next((m) => m.type === 'ack' && m.ref === `b${i}`);
    }
    const late = connect(gw.port);
    await late.ready();
    late.send({ op: 'subscribe', symbol: 'MSFT', ref: 's' });
    const snap = await late.next((m) => m.type === 'snapshot');
    assert.deepEqual(snap.bids, [[92, 7], [91, 6], [90, 5]]);

    // More activity after the snapshot: the next message continues from snap.seq.
    maker.send({ op: 'new', symbol: 'MSFT', side: 'buy', type: 'limit', price: 93, qty: 1, ref: 'z' });
    const first = await late.next((m) => m.type === 'level');
    assert.equal(first.seq, snap.seq + 1);
    assert.equal(first.price, 93);
    await maker.close();
    await late.close();
  });

  it('only lets the owner cancel an order', async () => {
    const a = connect(gw.port);
    const b = connect(gw.port);
    await a.ready();
    await b.ready();
    a.send({ op: 'new', symbol: 'AAPL', side: 'buy', type: 'limit', price: 50, qty: 1, ref: 'o' });
    const { id } = await a.next((m) => m.type === 'ack' && m.ref === 'o');
    b.send({ op: 'cancel', id, ref: 'c1' });
    assert.equal((await b.next((m) => m.ref === 'c1')).ok, false);
    a.send({ op: 'cancel', id, ref: 'c2' });
    assert.equal((await a.next((m) => m.ref === 'c2')).ok, true);
    await a.close();
    await b.close();
  });

  it('answers bad messages with an error and keeps running', async () => {
    const a = connect(gw.port);
    await a.ready();
    a.ws.send('not json');
    await a.next((m) => m.type === 'error' && m.reason === 'not JSON');
    a.send({ op: 'new', symbol: 'AAPL', side: 'buy', price: 1.5, qty: 1, ref: 'p' });
    await a.next((m) => m.type === 'error' && m.ref === 'p');
    a.send({ op: 'new', symbol: 'NOPE', side: 'buy', price: 1, qty: 1, ref: 'q' });
    assert.equal((await a.next((m) => m.ref === 'q')).reason, 'unknown symbol');
    a.send({ op: 'new', symbol: 'AAPL', side: 'buy; CANCEL', price: 1, qty: 1, ref: 'inj' });
    await a.next((m) => m.type === 'error' && m.ref === 'inj');
    // still works afterwards
    a.send({ op: 'ledger', ref: 'l' });
    assert.equal((await a.next((m) => m.type === 'ledger')).balanced, true);
    await a.close();
  });
});
