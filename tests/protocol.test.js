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
// plugin windows: Win32, or X11 on Linux when there is a display (CI runs the Linux tests under Xvfb)
const EDITORS = IS_WIN || (process.platform === "linux" && !!process.env.DISPLAY);

let proc, port, codeFile, out = "";

async function waitFor(cond, what, ms = 2000) {
  const end = Date.now() + ms;
  while (!cond()) {
    if (Date.now() > end) assert.fail("timed out waiting for " + what);
    await new Promise((r) => setTimeout(r, 25));
  }
}
const codes = [];

test.before(async () => {
  assert.ok(BRIDGE && PLUGINS, "set OROS_BRIDGE and OROS_TEST_PLUGINS");
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "oros-bridge-"));
  fs.writeFileSync(path.join(dir, "config.json"), JSON.stringify({ folders: [PLUGINS] }));
  codeFile = path.join(dir, "code.txt");
  const args = ["--console", "--port", "0", "--config-dir", dir, "--allow-origin", ORIGIN, "--test-audio", "--code-file", codeFile];
  if (process.env.OROS_TEST_AUDIO_MODULE) args.push("--audio-module", process.env.OROS_TEST_AUDIO_MODULE);
  proc = spawn(BRIDGE, args, { stdio: ["ignore", "pipe", "inherit"] });
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
  // the next binary message (a take block) and the answer to `o`
  async take(o) {
    const id = (this.id = (this.id || 0) + 1);
    this.send(JSON.stringify(Object.assign({ id, op: "takeRead" }, o)), 1);
    let bin = null;
    for (;;) {
      const m = await this.next();
      if (Buffer.isBuffer(m)) bin = m;
      else if (m && m.id === id) return { r: m, bin };
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
  assert.equal(hello.editors, EDITORS);
  assert.equal((await c.cmd({ op: "list" })).error, "not paired");
  assert.equal((await c.cmd({ op: "auth", token: "0".repeat(64) })).ok, false);
  assert.equal((await c.cmd({ op: "pair" })).shown, true);
  await new Promise((r) => setTimeout(r, 100));
  const code = codes[codes.length - 1];
  assert.match(code, /^\d{6}$/);
  // --code-file: the open code is in the file while it is open, and gone after
  await waitFor(() => fs.existsSync(codeFile), "the code file");
  assert.equal(fs.readFileSync(codeFile, "utf8"), code + "\n");
  const wrong = code === "000000" ? "111111" : "000000";
  assert.equal((await c.cmd({ op: "pair", code: wrong })).error, "wrong code");
  const r = await c.cmd({ op: "pair", code });
  assert.equal(r.ok, true);
  await waitFor(() => !fs.existsSync(codeFile), "the code file removed");
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
  // the gain has a window on Linux only (no Win32 one): the host's loop runs its timer and its pipe
  const LINUX_WIN = EDITORS && !IS_WIN;
  assert.equal(o.editor, LINUX_WIN);
  assert.equal((await c.cmd({ op: "editor", h: o.h })).ok, LINUX_WIN);
  if (LINUX_WIN) {
    await new Promise((r) => setTimeout(r, 300));
    const s1 = Buffer.from((await c.cmd({ op: "getState", h: o.h })).data, "base64");
    const ticks = s1.readUInt32LE(4), reads = s1.readUInt32LE(8), parent = s1.readUInt32LE(12);
    assert.ok(ticks >= 5, "timer ticks " + ticks);
    assert.ok(reads >= ticks - 2 && reads <= ticks, "pipe bytes read " + reads + " of " + ticks);
    assert.ok(parent > 0, "an X11 window id");
    assert.equal((await c.cmd({ op: "closeEditor", h: o.h })).ok, true);
    const t1 = Buffer.from((await c.cmd({ op: "getState", h: o.h })).data, "base64").readUInt32LE(4);
    await new Promise((r) => setTimeout(r, 150));
    const t2 = Buffer.from((await c.cmd({ op: "getState", h: o.h })).data, "base64").readUInt32LE(4);
    assert.equal(t2, t1, "no ticks once the window is closed");
  }
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

// ---- the sound card (the loopback card of --test-audio) ----
// input 1/2 = output 1/2 333 frames later, input 3/4 the same at half level
function playBlock(offset, chans) {
  const frames = chans[0].length, b = Buffer.alloc(16 + chans.length * frames * 4);
  b.write("ORP1", 0);
  b.writeUInt32LE(offset, 4); b.writeUInt32LE(frames, 8); b.writeUInt16LE(chans.length, 12);
  chans.forEach((ch, c) => ch.forEach((v, i) => b.writeFloatLE(v, 16 + (c * frames + i) * 4)));
  return b;
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

test("sound card: the backends, the loopback card listed first", async () => {
  const c = await paired();
  const r = await c.cmd({ op: "audioDevices" });
  assert.equal(r.ok, true);
  assert.equal(r.backends[0].id, "test");
  assert.deepEqual(r.backends[0].inputs, [{ name: "Loopback", channels: 4, default: true }]);
  const ids = r.backends.map((b) => b.id);
  if (IS_WIN) for (const id of ["wasapi", "asio"]) assert.ok(ids.includes(id), id);   // asio: oros-bridge-asio.dll beside the exe
  else if (process.platform === "linux") for (const id of ["jack", "pulse", "alsa"]) assert.ok(ids.includes(id), id);
  for (const b of r.backends) assert.ok(b.ok === true || typeof b.error === "string", b.id);
  c.close();
});

test("sound card: a take lines up with what was played, sample for sample", async () => {
  const c = await paired();
  assert.equal((await c.cmd({ op: "audioStart" })).error, "no sound card open");
  const o = await c.cmd({ op: "audioOpen", backend: "test", sr: 48000, period: 128, record: [0, 3] });
  assert.equal(o.ok, true, o.error);
  assert.deepEqual([o.sr, o.inputs, o.outputs, o.latency], [48000, 4, 2, 333]);
  const N = 12000, L = ramp(N), R = ramp(N, -0.5);
  assert.equal((await c.cmd({ op: "audioPlay", frames: N, channels: 2, out: 0 })).ok, true);
  c.send(playBlock(0, [L.slice(0, 5000), R.slice(0, 5000)]), 2);
  c.send(playBlock(5000, [L.slice(5000), R.slice(5000)]), 2);
  assert.equal((await c.cmd({ op: "audioStart" })).ok, true);
  assert.equal((await c.cmd({ op: "audioPlay", frames: 10, channels: 1 })).error, "playing");
  await sleep(500);   // 24000 frames
  const s = await c.cmd({ op: "audioStop" });
  assert.equal(s.ok, true, s.error);
  assert.equal(s.lost, 0);
  assert.ok(s.frames >= 20000, "recorded " + s.frames);
  const { r, bin } = await c.take({ offset: 0, frames: 20000 });
  assert.equal(r.frames, 20000);
  assert.equal(bin.slice(0, 4).toString(), "ORT1");
  assert.equal(bin.readUInt32LE(8), 20000);
  assert.equal(bin.readUInt16LE(12), 2);
  const at = (ch, i) => bin.readFloatLE(16 + (ch * 20000 + i) * 4);
  for (let i = 0; i < 333; i++) assert.equal(at(0, i), 0);
  for (let i = 0; i < N; i++) {
    assert.equal(at(0, 333 + i), L[i], "input 1 at " + i);
    assert.equal(at(1, 333 + i), Math.fround(R[i] * 0.5), "input 4 at " + i);
  }
  for (let i = 333 + N; i < 20000; i++) assert.equal(at(0, i), 0, "silence after the end");
  // a read past the end gives what is left
  const tail = await c.take({ offset: s.frames - 10, frames: 100 });
  assert.equal(tail.r.frames, 10);
  // levels come while the card is open
  let lv;
  for (;;) { const m = await c.next(); if (m && m.event === "levels") { lv = m; break; } }
  assert.equal(lv.peaks.length, 4);
  assert.equal(lv.running, false);
  // a second take starts over
  assert.equal((await c.cmd({ op: "audioStart" })).ok, true);
  await sleep(150);
  const s2 = await c.cmd({ op: "audioStop" });
  assert.ok(s2.frames > 0 && s2.frames < s.frames);
  assert.equal((await c.cmd({ op: "audioClose" })).ok, true);
  assert.equal((await c.cmd({ op: "takeRead", offset: 0, frames: 10 })).error, "no sound card open");
  c.close();
});

test("sound card: bad requests are refused", async () => {
  const c = await paired();
  assert.equal((await c.cmd({ op: "audioOpen", backend: "nope" })).error, "unknown backend");
  assert.equal((await c.cmd({ op: "audioOpen", backend: "test", input: "Other" })).ok, false);
  assert.match((await c.cmd({ op: "audioOpen", backend: "test", record: [4] })).error, /no input 5/);
  assert.equal((await c.cmd({ op: "audioOpen", backend: "test", record: "x" })).error, "bad inputs");
  assert.equal((await c.cmd({ op: "audioOpen", backend: "test", period: 4 })).ok, false);
  assert.equal((await c.cmd({ op: "audioOpen", backend: "test", record: [0] })).ok, true);
  assert.equal((await c.cmd({ op: "audioPlay", frames: 100, channels: 3 })).error, "bad channels");
  assert.equal((await c.cmd({ op: "audioPlay", frames: 100, channels: 2 })).ok, true);
  c.send(playBlock(90, [ramp(20), ramp(20)]), 2);
  let e;
  for (;;) { const m = await c.next(); if (m && m.event === "audioError") { e = m; break; } }
  assert.equal(e.error, "bad block");
  assert.equal((await c.cmd({ op: "audioStop" })).error, "not playing");
  c.close();   // closing the page closes the card
  const c2 = await paired();
  assert.equal((await c2.cmd({ op: "audioOpen", backend: "test", record: [0] })).ok, true);
  c2.close();
});

// ---- a plugin live on an input (0.5.0) ----
// The test gain (latency 32, gain 0.5) live on loopback input 3 (= output 1 at
// half level, 333 frames later), heard on outputs 1/2: what reaches output 1
// comes back through the plugin, so out[u] = play[u] + 0.25 * out[u - 365].
function liveOut(P, n) {
  const O = new Float32Array(n);
  for (let u = 0; u < n; u++) {
    const back = u >= 365 ? Math.fround(Math.fround(0.5 * Math.fround(0.5 * O[u - 365])) * 1) : 0;
    O[u] = Math.fround((u < P.length ? P[u] : 0) + back);
  }
  return O;
}
async function liveTake(c, N, n) {
  const L = ramp(N);
  assert.equal((await c.cmd({ op: "audioPlay", frames: N, channels: 1, out: 0 })).ok, true);
  for (let at = 0; at < N; at += 4000) c.send(playBlock(at, [L.slice(at, at + 4000)]), 2);
  assert.equal((await c.cmd({ op: "audioStart" })).ok, true);
  await sleep(600);
  const s = await c.cmd({ op: "audioStop" });
  assert.equal(s.ok, true, s.error);
  assert.ok(s.frames >= n, "recorded " + s.frames);
  const { bin } = await c.take({ offset: 0, frames: n });
  return { L, at: (ch, i) => bin.readFloatLE(16 + (ch * n + i) * 4) };
}

test("live plugin: heard on the outputs, a dry take, a wet take, off again", async () => {
  const c = await paired();
  await c.cmd({ op: "scan" });
  assert.equal((await c.cmd({ op: "liveOpen", plugin: "clap:oros.test.gain", inputs: [2] })).error, "no sound card open");
  const o = await c.cmd({ op: "audioOpen", backend: "test", sr: 48000, period: 128, record: [0, 2] });
  assert.equal(o.ok, true, o.error);
  const lv = await c.cmd({ op: "liveOpen", plugin: "clap:oros.test.gain", inputs: [2], out: 0, gain: 1, on: true });
  assert.equal(lv.ok, true, lv.error);
  assert.deepEqual([lv.latency, lv.monitor, lv.inputs, lv.outputs, lv.on, lv.record], [32, 365, 2, 2, true, "dry"]);
  // a dry take: input 1 records output 1, which carries the plugin's sound
  const N = 9000, n = 333 + N + 2000;
  let t = await liveTake(c, N, n);
  let O = liveOut(t.L, n);
  for (let i = 0; i < 333; i++) assert.equal(t.at(0, i), 0);
  for (let i = 333; i < n; i++) assert.ok(close(t.at(0, i), O[i - 333]), "dry input 1 at " + i + ": " + t.at(0, i) + " vs " + O[i - 333]);
  for (let i = 333; i < n; i++) assert.ok(close(t.at(1, i), Math.fround(0.5 * O[i - 333])), "dry input 3 at " + i);
  // the meters show the plugin
  let ev;
  for (;;) { const m = await c.next(); if (m && m.event === "levels" && m.live) { ev = m; break; } }
  assert.equal(ev.live.h, lv.h);
  assert.equal(ev.live.on, true);
  assert.equal(ev.live.slow, false);
  // its window, state and info work on the live one; a render or reset does not
  assert.equal((await c.cmd({ op: "info", h: lv.h })).latency, 32);
  assert.equal((await c.cmd({ op: "getState", h: lv.h })).ok, true);
  assert.match((await c.cmd({ op: "reset", h: lv.h })).error, /live/);
  const r = await c.render(lv.h, [[0.5]], 1);
  assert.match(r.error, /live/);
  assert.match((await c.cmd({ op: "liveOpen", plugin: "clap:oros.test.sine", inputs: [0] })).error, /MIDI keyboard/);
  // a wet take: input 3 records the plugin's sound instead (state from the page: gain 0.5)
  const st = Buffer.alloc(4); st.writeFloatLE(0.5, 0);
  const wv = await c.cmd({ op: "liveOpen", plugin: "clap:oros.test.gain", inputs: [2], out: 0, on: true, record: "wet", state: st.toString("base64") });
  assert.equal(wv.ok, true, wv.error);
  assert.equal((await c.cmd({ op: "info", h: lv.h })).error, "unknown handle");   // a new live plugin replaces the first
  t = await liveTake(c, N, n);
  O = liveOut(t.L, n);
  for (let i = 333; i < n; i++) assert.ok(close(t.at(0, i), O[i - 333]), "input 1 at " + i);
  for (let i = 365; i < n; i++) assert.ok(close(t.at(1, i), Math.fround(0.25 * O[i - 365])), "wet input 3 at " + i + ": " + t.at(1, i));
  // off: nothing comes back, input 1 is what was played
  const off = await c.cmd({ op: "liveSet", on: false, gain: 0.7 });
  assert.deepEqual([off.on, Math.fround(off.gain)], [false, Math.fround(0.7)]);
  t = await liveTake(c, N, n);
  for (let i = 0; i < N; i++) assert.equal(t.at(0, 333 + i), t.L[i], "off: input 1 at " + i);
  assert.equal((await c.cmd({ op: "liveSet", gain: 9 })).error, "bad gain");
  assert.equal((await c.cmd({ op: "liveOpen", plugin: "clap:oros.test.gain", inputs: [9] })).error, "bad inputs");
  assert.equal((await c.cmd({ op: "liveOpen", plugin: "clap:oros.test.gain", inputs: [0], record: "x" })).error, "bad record");
  // closing it by its handle takes it off the inputs
  assert.equal((await c.cmd({ op: "close", h: wv.h })).ok, true);
  assert.equal((await c.cmd({ op: "liveSet", on: true })).error, "no live plugin");
  // a VST 3 live too; closing the card closes it
  const v3 = (await c.cmd({ op: "list" })).plugins.find((p) => p.format === "vst3" && !p.instrument);
  const lv3 = await c.cmd({ op: "liveOpen", plugin: v3.id, inputs: [0, 1], on: false });
  assert.equal(lv3.ok, true, lv3.error);
  assert.equal((await c.cmd({ op: "audioClose" })).ok, true);
  assert.equal((await c.cmd({ op: "info", h: lv3.h })).error, "unknown handle");
  c.close();
});

// ---- an instrument live from a MIDI keyboard the bridge reads (0.6.0) ----
// "Test MIDI" (--test-audio) plays what midiTest hands in; the test sine
// sounds a = velocity / 4 per held note on outputs 1/2, which the loopback
// card brings back on input 1 333 frames later.
test("live instrument: a MIDI keyboard plays it on the card, the take keeps the notes", async () => {
  const c = await paired();
  const md = await c.cmd({ op: "midiDevices" });
  assert.equal(md.ok, true);
  assert.equal(md.inputs[0], "Test MIDI");
  await c.cmd({ op: "scan" });
  assert.equal((await c.cmd({ op: "audioOpen", backend: "test", sr: 48000, period: 128, record: [0] })).ok, true);
  assert.match((await c.cmd({ op: "liveOpen", plugin: "clap:oros.test.sine" })).error, /MIDI keyboard/);
  assert.match((await c.cmd({ op: "liveOpen", plugin: "clap:oros.test.sine", midi: "No such" })).error, /no such MIDI input/);
  const lv = await c.cmd({ op: "liveOpen", plugin: "clap:oros.test.sine", midi: "Test MIDI", on: true });
  assert.equal(lv.ok, true, lv.error);
  assert.deepEqual([lv.instrument, lv.midi, lv.outputs], [true, "Test MIDI", 2]);
  assert.equal((await c.cmd({ op: "audioPlay", frames: 0, channels: 0 })).ok, true);
  assert.equal((await c.cmd({ op: "audioStart" })).ok, true);
  await sleep(150);
  assert.equal((await c.cmd({ op: "midiTest", bytes: [0x90, 69, 127] })).ok, true);   // note on, a = 0.25
  await sleep(250);
  assert.equal((await c.cmd({ op: "midiTest", bytes: [69, 0] })).ok, true);           // running status: velocity 0 = off
  await sleep(200);
  const s = await c.cmd({ op: "audioStop" });
  assert.equal(s.ok, true, s.error);
  const n = s.frames;
  const { bin } = await c.take({ offset: 0, frames: n });
  const at = (i) => bin.readFloatLE(16 + i * 4);
  const mt = await c.cmd({ op: "midiTake" });
  assert.equal(mt.ok, true, mt.error);
  assert.deepEqual(mt.events.map((e) => e.slice(1)), [[0x90, 69, 127], [0x90, 69, 0]]);
  const [on, off] = mt.events.map((e) => e[0]);
  assert.ok(on > 4000 && on < off && off < n, "frames " + on + " " + off);
  // silence before the note, a 440 Hz sine at 0.25 while it is held, silence after
  let pk = 0;
  for (let i = 0; i < on + 333; i++) pk = Math.max(pk, Math.abs(at(i)));
  assert.equal(pk, 0, "silent before the note");
  pk = 0;
  for (let i = on + 333 + 200; i < off + 333 - 200; i++) pk = Math.max(pk, Math.abs(at(i)));
  assert.ok(pk > 0.24 && pk <= 0.25 + 1e-6, "peak " + pk);
  pk = 0;
  for (let i = off + 333 + 200; i < n; i++) pk = Math.max(pk, Math.abs(at(i)));
  assert.equal(pk, 0, "silent after the note");
  // the sound starts where the take says the note came
  let first = on + 333;
  while (first < n && at(first) === 0) first++;
  assert.ok(first - (on + 333) <= 1, "starts at " + first + ", note at " + (on + 333));
  assert.equal((await c.cmd({ op: "audioClose" })).ok, true);
  assert.equal((await c.cmd({ op: "midiTest", bytes: [0x90, 60, 1] })).error, "no live keyboard");
  c.close();
});

// ---- a sound system from a separate library (src/audio_module.h) ----
// the test module: input 1/2 = output 1/2 100 frames later, input 3 = output 2 at a quarter
async function lineUp(c, open, { delay, inputs, check }) {
  const o = await c.cmd(Object.assign({ op: "audioOpen" }, open));
  assert.equal(o.ok, true, o.error);
  assert.equal(o.inputs, inputs);
  assert.equal(o.latency, delay);
  const N = 9000, L = ramp(N), R = ramp(N, -0.5);
  assert.equal((await c.cmd({ op: "audioPlay", frames: N, channels: 2, out: 0 })).ok, true);
  for (let at = 0; at < N; at += 4000) c.send(playBlock(at, [L.slice(at, at + 4000), R.slice(at, at + 4000)]), 2);
  assert.equal((await c.cmd({ op: "audioStart" })).ok, true);
  await sleep(600);
  const s = await c.cmd({ op: "audioStop" });
  assert.equal(s.ok, true, s.error);
  assert.equal(s.lost, 0);
  const n = delay + N + 100;
  assert.ok(s.frames >= n, "recorded " + s.frames);
  const { bin } = await c.take({ offset: 0, frames: n });
  const at = (ch, i) => bin.readFloatLE(16 + (ch * n + i) * 4);
  for (let i = 0; i < delay; i++) assert.equal(at(0, i), 0);
  for (let i = 0; i < N; i++) check(at(0, delay + i), L[i], at(1, delay + i), R[i], i);
  for (let i = delay + N; i < n; i++) assert.equal(at(0, i), 0, "silence after the end");
  assert.equal((await c.cmd({ op: "audioClose" })).ok, true);
}

test("audio module: offered as a backend, a take lines up sample for sample", { skip: !process.env.OROS_TEST_AUDIO_MODULE }, async () => {
  const c = await paired();
  const r = await c.cmd({ op: "audioDevices" });
  const m = r.backends.find((b) => b.id === "modtest");
  assert.ok(m, "the module's backend is listed");
  assert.equal(m.label, "Module loopback (test)");
  assert.equal(m.ok, true);
  assert.deepEqual(m.inputs, [{ name: "Module loopback", channels: 3, default: true }]);
  assert.equal((await c.cmd({ op: "audioOpen", backend: "modtest", input: "Other" })).error, "no such card: Other");
  await lineUp(c, { backend: "modtest", sr: 44100, period: 64, record: [0, 2] }, {
    delay: 100, inputs: 3,
    check: (a, l, b, rr, i) => { assert.equal(a, l, "input 1 at " + i); assert.equal(b, Math.fround(rr * 0.25), "input 3 at " + i); },
  });
  c.close();
});

// ---- ASIO (Windows): oros-bridge-asio.dll with the loopback driver asio/test_driver.cpp ----
// The driver is registered the way installers do (HKLM\SOFTWARE\ASIO + its COM class), which needs
// an administrator: CI runs as one, elsewhere the test is skipped.
const ASIO_DRIVER = process.env.OROS_TEST_ASIO_DRIVER;
const ASIO_CLSID = "{F20028E5-5EE8-4D40-AE0B-AB0066A32676}";
const ASIO_KEY = "HKLM\\SOFTWARE\\ASIO\\orOS Test ASIO";
const COM_KEY = "HKLM\\Software\\Classes\\CLSID\\" + ASIO_CLSID;
function reg(args) {
  return require("node:child_process").spawnSync("reg", args, { encoding: "utf8" }).status === 0;
}

test("ASIO: the loopback driver is listed, a take lines up with what was played", { skip: !(IS_WIN && ASIO_DRIVER) }, async (t) => {
  if (!reg(["add", ASIO_KEY, "/v", "CLSID", "/d", ASIO_CLSID, "/f"])) { t.skip("not an administrator"); return; }
  t.after(() => { reg(["delete", ASIO_KEY, "/f"]); reg(["delete", COM_KEY, "/f"]); });
  assert.ok(reg(["add", ASIO_KEY, "/v", "Description", "/d", "orOS Test ASIO", "/f"]));
  assert.ok(reg(["add", COM_KEY + "\\InprocServer32", "/ve", "/d", path.resolve(ASIO_DRIVER), "/f"]));
  assert.ok(reg(["add", COM_KEY + "\\InprocServer32", "/v", "ThreadingModel", "/d", "Apartment", "/f"]));
  const c = await paired();
  const r = await c.cmd({ op: "audioDevices" });
  const a = r.backends.find((b) => b.id === "asio");
  assert.equal(a.ok, true, a.error);
  assert.equal(a.label, "ASIO");
  const card = a.inputs.find((d) => d.name === "orOS Test ASIO");
  assert.ok(card, JSON.stringify(a.inputs));
  assert.equal(card.channels, 4);
  assert.equal(a.outputs.find((d) => d.name === "orOS Test ASIO").channels, 2);
  assert.match((await c.cmd({ op: "audioOpen", backend: "asio", input: "orOS Test ASIO", output: "Other" })).error, /same card/);
  assert.match((await c.cmd({ op: "audioOpen", backend: "asio", input: "No such" })).error, /no such ASIO driver/);
  // 32-bit integer samples there and back: within one step of 2^-31
  const near = (x, y, what) => assert.ok(Math.abs(x - y) < 1e-6, what + ": " + x + " vs " + y);
  await lineUp(c, { backend: "asio", input: "orOS Test ASIO", output: "orOS Test ASIO", sr: 48000, record: [0, 3] }, {
    delay: 480, inputs: 4,
    check: (x, l, y, rr, i) => { near(x, l, "input 1 at " + i); near(y, rr * 0.5, "input 4 at " + i); },
  });
  // one driver per process: a second page waits until the first closes
  assert.equal((await c.cmd({ op: "audioOpen", backend: "asio", input: "orOS Test ASIO", record: [0] })).ok, true);
  const c2 = await paired();
  assert.match((await c2.cmd({ op: "audioOpen", backend: "asio", input: "orOS Test ASIO" })).error, /already in use/);
  const again = (await c2.cmd({ op: "audioDevices" })).backends.find((b) => b.id === "asio");
  assert.ok(again.inputs.some((d) => d.name === "orOS Test ASIO"), "the list stays while a card is open");
  c.close();
  await sleep(300);
  assert.equal((await c2.cmd({ op: "audioOpen", backend: "asio", input: "orOS Test ASIO" })).ok, true);
  c2.close();
});
