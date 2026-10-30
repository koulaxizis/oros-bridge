// The bridge's protocol, end to end: a real oros-bridge process, the test
// plugins (oros-test.clap, oros-test.vst3) and a WebSocket client written
// here so the test controls every header.
//   OROS_BRIDGE=<oros-bridge> OROS_TEST_PLUGINS=<folder> node --test tests/protocol.test.js
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const { spawn } = require("node:child_process");
const crypto = require("node:crypto");
const fs = require("node:fs");
const net = require("node:net");
const os = require("node:os");
const path = require("node:path");

const BRIDGE = process.env.OROS_BRIDGE;
const PLUGINS = process.env.OROS_TEST_PLUGINS;
const ORIGIN = "http://test.local";
const IS_WIN = process.platform === "win32";

let proc, port, out = "";
const codes = [];

test.before(async () => {
  assert.ok(BRIDGE && PLUGINS, "set OROS_BRIDGE and OROS_TEST_PLUGINS");
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "oros-bridge-"));
  fs.writeFileSync(path.join(dir, "config.json"), JSON.stringify({ folders: [PLUGINS] }));
  proc = spawn(BRIDGE, ["--port", "0", "--config-dir", dir, "--allow-origin", ORIGIN], { stdio: ["ignore", "pipe", "inherit"] });
  await new Promise((res, rej) => {
    proc.on("exit", (c) => rej(new Error("bridge exited " + c)));
    proc.stdout.on("data", (d) => {
      out += d;
      let m;
      const re = /Pairing code: (\d{6})/g;
      codes.length = 0;
      while ((m = re.exec(out))) codes.push(m[1]);
      if ((m = /ready on 127\.0\.0\.1:(\d+)/.exec(out)) && !port) { port = +m[1]; res(); }
    });
  });
});
test.after(() => { if (proc) proc.kill(); });

// ---- a small HTTP/WebSocket client ----
function rawRequest(lines) {
  return new Promise((res) => {
    const s = net.connect(port, "127.0.0.1", () => s.write(lines.join("\r\n") + "\r\n\r\n"));
    let buf = "";
    s.on("data", (d) => { buf += d; });
    s.on("end", () => res(buf));
    s.on("error", () => res(buf));
  });
}
function status(text) { return +(/^HTTP\/1\.1 (\d+)/.exec(text) || [])[1]; }

class Client {
  static open({ origin = ORIGIN, host = "127.0.0.1:" + port } = {}) {
    return new Promise((res, rej) => {
      const c = new Client();
      const key = crypto.randomBytes(16).toString("base64");
      c.s = net.connect(port, "127.0.0.1", () => {
        c.s.write(["GET / HTTP/1.1", "Host: " + host, "Origin: " + origin, "Upgrade: websocket", "Connection: Upgrade",
          "Sec-WebSocket-Key: " + key, "Sec-WebSocket-Version: 13", "", ""].join("\r\n"));
      });
      let head = Buffer.alloc(0), upgraded = false;
      c.buf = Buffer.alloc(0);
      c.queue = [];
      c.waiters = [];
      c.s.on("data", (d) => {
        if (!upgraded) {
          head = Buffer.concat([head, d]);
          const i = head.indexOf("\r\n\r\n");
          if (i < 0) return;
          const text = head.slice(0, i).toString();
          if (status(text) !== 101) { c.s.destroy(); rej(new Error(text.split("\r\n")[0])); return; }
          const accept = /Sec-WebSocket-Accept: (\S+)/i.exec(text)[1];
          assert.equal(accept, crypto.createHash("sha1").update(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64"));
          upgraded = true;
          d = head.slice(i + 4);
          res(c);
        }
        c.buf = Buffer.concat([c.buf, d]);
        c.parse();
      });
      c.s.on("error", rej);
    });
  }
  parse() {
    for (;;) {
      const b = this.buf;
      if (b.length < 2) return;
      let len = b[1] & 127, at = 2;
      if (len === 126) { if (b.length < 4) return; len = b.readUInt16BE(2); at = 4; }
      else if (len === 127) { if (b.length < 10) return; len = Number(b.readBigUInt64BE(2)); at = 10; }
      if (b.length < at + len) return;
      const op = b[0] & 15, payload = b.slice(at, at + len);
      this.buf = b.slice(at + len);
      const msg = op === 1 ? JSON.parse(payload.toString()) : payload;
      const w = this.waiters.shift();
      if (w) w(msg); else this.queue.push(msg);
    }
  }
  next() { return this.queue.length ? Promise.resolve(this.queue.shift()) : new Promise((r) => this.waiters.push(r)); }
  send(data, op) {
    const p = typeof data === "string" ? Buffer.from(data) : data;
    const mask = crypto.randomBytes(4);
    let h;
    if (p.length < 126) h = Buffer.from([0x80 | op, 0x80 | p.length]);
    else if (p.length < 65536) { h = Buffer.alloc(4); h[0] = 0x80 | op; h[1] = 0x80 | 126; h.writeUInt16BE(p.length, 2); }
    else { h = Buffer.alloc(10); h[0] = 0x80 | op; h[1] = 0x80 | 127; h.writeBigUInt64BE(BigInt(p.length), 2); }
    const m = Buffer.from(p);
    for (let i = 0; i < m.length; i++) m[i] ^= mask[i & 3];
    this.s.write(Buffer.concat([h, mask, m]));
  }
  // a command; events that come first (hello, scanning…) are skipped
  async cmd(o) {
    const id = (this.id = (this.id || 0) + 1);
    this.send(JSON.stringify(Object.assign({ id }, o)), 1);
    for (;;) {
      const m = await this.next();
      if (m && m.id === id) return m;
    }
  }
  async render(h, chans, frames, events = []) {
    const nch = chans.length;
    const b = Buffer.alloc(20 + events.length * 8 + nch * frames * 4);
    b.write("ORB1", 0);
    b.writeUInt32LE(h, 4); b.writeUInt32LE(7, 8); b.writeUInt32LE(frames, 12);
    b.writeUInt16LE(nch, 16); b.writeUInt16LE(events.length, 18);
    events.forEach((e, i) => { const at = 20 + i * 8; b.writeUInt32LE(e[0], at); b[at + 4] = e[1]; b[at + 5] = e[2]; b[at + 6] = e[3]; });
    const a = 20 + events.length * 8;
    chans.forEach((ch, c) => ch.forEach((v, i) => b.writeFloatLE(v, a + (c * frames + i) * 4)));
    this.send(b, 2);
    for (;;) {
      const m = await this.next();
      if (Buffer.isBuffer(m)) {
        assert.equal(m.slice(0, 4).toString(), "ORB1");
        assert.equal(m.readUInt32LE(4), h);
        assert.equal(m.readUInt32LE(8), 7);
        const outs = m[16], n = m.readUInt32LE(12), res = [];
        for (let c = 0; c < outs; c++) { const ch = []; for (let i = 0; i < n; i++) ch.push(m.readFloatLE(20 + (c * n + i) * 4)); res.push(ch); }
        return res;
      }
      if (m.event === "renderError") return m;
    }
  }
  close() { this.s.destroy(); }
}

let token;
async function paired() {
  const c = await Client.open();
  assert.equal((await c.cmd({ op: "auth", token })).ok, true);
  return c;
}
const ramp = (n, k = 1) => Array.from({ length: n }, (_, i) => Math.fround(((i % 50) - 25) / 50 * k));
const close = (a, b, tol = 1e-6) => Math.abs(a - b) <= tol;

// ---- tests ----
test("status: allowed origin only, on 127.0.0.1 or localhost only", async () => {
  const ok = await rawRequest(["GET /status HTTP/1.1", "Host: 127.0.0.1:" + port, "Origin: " + ORIGIN]);
  assert.equal(status(ok), 200);
  assert.match(ok, /Access-Control-Allow-Origin: http:\/\/test\.local/);
  assert.match(ok, /Access-Control-Allow-Private-Network: true/);
  assert.match(ok, /"app":"oros-bridge"/);
  assert.equal(status(await rawRequest(["GET /status HTTP/1.1", "Host: 127.0.0.1:" + port, "Origin: https://evil.example"])), 403);
  assert.equal(status(await rawRequest(["GET /status HTTP/1.1", "Host: evil.example:" + port, "Origin: " + ORIGIN])), 403, "DNS rebinding");
  assert.equal(status(await rawRequest(["GET /status HTTP/1.1", "Host: localhost:" + port, "Origin: https://useoros.online"])), 200);
});

test("websocket: refused from another origin or host", async () => {
  await assert.rejects(Client.open({ origin: "https://evil.example" }), /403/);
  await assert.rejects(Client.open({ origin: "null" }), /403/);
  await assert.rejects(Client.open({ host: "evil.example:" + port }), /403/);
});

test("pairing: nothing before it, a wrong code fails, the right one gives a token", async () => {
  const c = await Client.open();
  const hello = await c.next();
  assert.equal(hello.event, "hello");
  assert.equal(hello.editors, IS_WIN);
  assert.equal((await c.cmd({ op: "list" })).error, "not paired");
  assert.equal((await c.cmd({ op: "auth", token: "0".repeat(64) })).ok, false);
  assert.equal((await c.cmd({ op: "pair" })).shown, true);
  await new Promise((r) => setTimeout(r, 100));
  const code = codes[codes.length - 1];
  assert.match(code, /^\d{6}$/);
  const wrong = code === "000000" ? "111111" : "000000";
  assert.equal((await c.cmd({ op: "pair", code: wrong })).error, "wrong code");
  const r = await c.cmd({ op: "pair", code });
  assert.equal(r.ok, true);
  assert.match(r.token, /^[0-9a-f]{64}$/);
  token = r.token;
  assert.equal((await c.cmd({ op: "list" })).ok, true);
  assert.equal((await c.cmd({ op: "pair", code })).ok, false, "a code works once");
  c.close();
});

test("pairing: three wrong codes end the code", async () => {
  const c = await Client.open();
  await c.cmd({ op: "pair" });
  await new Promise((r) => setTimeout(r, 100));
  const code = codes[codes.length - 1];
  const wrong = code === "000000" ? "111111" : "000000";
  await c.cmd({ op: "pair", code: wrong });
  await c.cmd({ op: "pair", code: wrong });
  assert.match((await c.cmd({ op: "pair", code: wrong })).error, /new one/);
  assert.equal((await c.cmd({ op: "pair", code })).ok, false, "the right code is gone too");
  c.close();
});

test("scan finds the test plugins in both formats", async () => {
  const c = await paired();
  const r = await c.cmd({ op: "scan", full: true });
  assert.equal(r.ok, true);
  const ids = r.plugins.map((p) => p.id).sort();
  assert.deepEqual(ids.filter((i) => i.startsWith("clap:")), ["clap:oros.test.gain", "clap:oros.test.sine"]);
  assert.equal(ids.filter((i) => i.startsWith("vst3:")).length, 1);
  assert.equal(r.plugins.find((p) => p.id === "clap:oros.test.sine").instrument, true);
  assert.deepEqual((await c.cmd({ op: "list" })).plugins.map((p) => p.id).sort(), ids);
  c.close();
});

test("CLAP effect: latency, render, state round trip, reset", async () => {
  const c = await paired();
  const o = await c.cmd({ op: "open", plugin: "clap:oros.test.gain", sr: 48000, block: 256 });
  assert.equal(o.ok, true, o.error);
  assert.equal(o.latency, 32);
  assert.equal(o.inputs, 2);
  assert.equal(o.outputs, 2);
  const L = ramp(256), R = ramp(256, -1);
  let out = await c.render(o.h, [L, R], 256);
  for (let i = 0; i < 256; i++) {
    assert.ok(close(out[0][i], i < 32 ? 0 : L[i - 32] * 0.5), "L " + i);
    assert.ok(close(out[1][i], i < 32 ? 0 : R[i - 32] * 0.5), "R " + i);
  }
  const st = await c.cmd({ op: "getState", h: o.h });
  assert.equal(Buffer.from(st.data, "base64").readFloatLE(0), 0.5);
  const quarter = Buffer.alloc(4); quarter.writeFloatLE(0.25);
  assert.equal((await c.cmd({ op: "setState", h: o.h, data: quarter.toString("base64") })).ok, true);
  assert.equal((await c.cmd({ op: "reset", h: o.h })).ok, true);
  out = await c.render(o.h, [L], 256);   // mono in: both sides
  for (let i = 32; i < 256; i++) assert.ok(close(out[1][i], L[i - 32] * 0.25));
  assert.equal((await c.cmd({ op: "editor", h: o.h })).ok, IS_WIN ? false : false, "the test plugin has no window");
  assert.equal((await c.cmd({ op: "close", h: o.h })).ok, true);
  assert.equal((await c.cmd({ op: "info", h: o.h })).error, "unknown handle");
  c.close();
});

test("CLAP instrument: notes at their frame", async () => {
  const c = await paired();
  const o = await c.cmd({ op: "open", plugin: "clap:oros.test.sine", sr: 48000, block: 1024 });
  assert.equal(o.ok, true, o.error);
  assert.equal(o.inputs, 0);
  assert.equal(o.instrument, true);
  const out = await c.render(o.h, [], 1024, [[100, 0x90, 69, 127], [900, 0x80, 69, 0]]);
  for (let i = 0; i < 100; i++) assert.equal(out[0][i], 0);
  for (let i = 900; i < 1024; i++) assert.equal(out[0][i], 0);
  let peak = 0, crossings = 0;
  for (let i = 101; i < 900; i++) {
    peak = Math.max(peak, Math.abs(out[0][i]));
    if ((out[0][i - 1] < 0) !== (out[0][i] < 0)) crossings++;
  }
  assert.ok(close(peak, 0.25, 0.01), "velocity 127 -> 0.25, got " + peak);
  const hz = crossings / 2 / (800 / 48000);
  assert.ok(Math.abs(hz - 440) < 30, "about 440 Hz, got " + hz);
  c.close();
});

test("VST3 effect: render, parameter state through the bridge's container", async () => {
  const c = await paired();
  const id = (await c.cmd({ op: "list" })).plugins.find((p) => p.format === "vst3").id;
  const o = await c.cmd({ op: "open", plugin: id, sr: 44100, block: 128 });
  assert.equal(o.ok, true, o.error);
  assert.equal(o.latency, 0);
  const L = ramp(128);
  let out = await c.render(o.h, [L, L], 128);
  for (let i = 0; i < 128; i++) assert.ok(close(out[0][i], L[i] * 0.5));
  const st = Buffer.from((await c.cmd({ op: "getState", h: o.h })).data, "base64");
  assert.equal(st.slice(0, 4).toString(), "OBV3");
  assert.equal(st.readUInt32LE(4), 4);
  assert.equal(st.readFloatLE(8), 0.5);
  st.writeFloatLE(1, 8);
  assert.equal((await c.cmd({ op: "setState", h: o.h, data: st.toString("base64") })).ok, true);
  out = await c.render(o.h, [L, L], 128);
  for (let i = 0; i < 128; i++) assert.ok(close(out[0][i], L[i]));
  assert.equal((await c.cmd({ op: "setState", h: o.h, data: Buffer.from("nope").toString("base64") })).ok, false);
  c.close();
});

test("bad requests are refused, the connection lives on", async () => {
  const c = await paired();
  assert.equal((await c.cmd({ op: "open", plugin: "clap:nope" })).error, "unknown plugin");
  assert.equal((await c.cmd({ op: "open", plugin: "clap:oros.test.gain", sr: 1000 })).ok, false);
  const o = await c.cmd({ op: "open", plugin: "clap:oros.test.gain", sr: 48000, block: 64 });
  const big = await c.render(o.h, [ramp(128)], 128);
  assert.equal(big.event, "renderError");
  assert.equal((await c.render(99, [ramp(16)], 16)).error, "unknown handle");
  assert.equal((await c.cmd({ op: "nope" })).error, "unknown handle");
  assert.equal((await c.cmd({ op: "setFolders", extra: "x" })).ok, false);
  c.send("{not json", 1);
  assert.equal((await c.next()).error, "bad json");
  const again = await c.render(o.h, [ramp(64)], 64);
  assert.equal(again.length, 2);
  c.close();
});
