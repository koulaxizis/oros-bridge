// Bridge updates (0.8.0, DESIGN.md decision 15): a copy of the built bridge
// in a temporary folder asks a local stand-in for GitHub (OROS_BRIDGE_UPDATE_URL)
// and updates itself: the exe (and the ASIO module beside it) on Windows,
// the program from the tar.gz on Linux. Every file must match SHA256SUMS.txt.
//   OROS_BRIDGE=<oros-bridge> node --test tests/update.test.js
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const { spawn, execFileSync } = require("node:child_process");
const crypto = require("node:crypto");
const fs = require("node:fs");
const http = require("node:http");
const os = require("node:os");
const path = require("node:path");

const BRIDGE = process.env.OROS_BRIDGE;
const IS_WIN = process.platform === "win32";
const LINUX = process.platform === "linux";
const sha = (b) => crypto.createHash("sha256").update(b).digest("hex");

// the stand-in: /latest and /download/<tag>/<asset>
let server, base, release = { tag: "v0.0.1", files: {}, sums: null, prerelease: false };
test.before(async () => {
  assert.ok(BRIDGE, "set OROS_BRIDGE");
  server = http.createServer((req, res) => {
    if (req.url === "/latest") {
      res.setHeader("content-type", "application/json");
      res.end(JSON.stringify({ tag_name: release.tag, draft: false, prerelease: release.prerelease }));
      return;
    }
    const m = /^\/download\/([^/]+)\/(.+)$/.exec(req.url);
    if (m && m[1] === release.tag) {
      if (m[2] === "SHA256SUMS.txt") {
        const sums = release.sums || Object.entries(release.files).map(([n, b]) => `${sha(b)}  ${n}`).join("\r\n") + "\r\n";
        res.end(sums);
        return;
      }
      if (release.files[m[2]]) { res.end(release.files[m[2]]); return; }
    }
    res.statusCode = 404;
    res.end("not found");
  });
  await new Promise((r) => server.listen(0, "127.0.0.1", r));
  base = `http://127.0.0.1:${server.address().port}`;
});
test.after(() => server && server.close());

// a copy of the bridge that may replace itself; config in its own folder
function copy() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "oros-bridge-update-"));
  const exe = path.join(dir, IS_WIN ? "oros-bridge.exe" : "oros-bridge");
  fs.copyFileSync(BRIDGE, exe);
  fs.chmodSync(exe, 0o755);
  return { dir, exe };
}
// runs the bridge without blocking the event loop (the stand-in answers meanwhile)
function runAsync(exe, args, dir) {
  const env = { ...process.env, OROS_BRIDGE_UPDATE_URL: base };
  delete env.APPIMAGE;
  return new Promise((res) => {
    const p = spawn(exe, [...args, "--config-dir", path.join(dir, "cfg")], { env });
    let out = "", err = "";
    p.stdout.on("data", (d) => (out += d));
    p.stderr.on("data", (d) => (err += d));
    p.on("exit", (status) => res({ status, out, err }));
  });
}

// what an update brings for this platform
function newRelease(tag, dir) {
  const files = {};
  if (IS_WIN) {
    files["oros-bridge-windows-x64.exe"] = Buffer.from("new exe " + tag);
    files["oros-bridge-asio.dll"] = Buffer.from("new asio module " + tag);
  } else {
    const src = path.join(dir, "src");
    fs.mkdirSync(path.join(src, "oros-bridge"), { recursive: true });
    fs.writeFileSync(path.join(src, "oros-bridge", "oros-bridge"), "#!/bin/sh\necho new " + tag + "\n");
    fs.writeFileSync(path.join(src, "oros-bridge", "README.md"), "readme");
    execFileSync("tar", ["-czf", path.join(dir, "rel.tar.gz"), "-C", src, "oros-bridge"]);
    files["oros-bridge-linux-x64.tar.gz"] = fs.readFileSync(path.join(dir, "rel.tar.gz"));
  }
  files["oros-bridge-linux-x64.deb"] = Buffer.from("not used here");
  return { tag, files, sums: null, prerelease: false };
}
const expected = (tag) => IS_WIN ? "new exe " + tag : "#!/bin/sh\necho new " + tag + "\n";

test("check: an older or equal release is up to date, a newer one is named, a prerelease is not used", async () => {
  const { dir, exe } = copy();
  release = { tag: "v0.0.1", files: {} };
  let r = await runAsync(exe, ["--check-update"], dir);
  assert.equal(r.status, 0, r.err);
  assert.match(r.out, /is up to date/);
  release = { tag: "v99.0.0", files: {} };
  r = await runAsync(exe, ["--check-update"], dir);
  assert.equal(r.status, 0, r.err);
  assert.match(r.out, /orOS Bridge 99\.0\.0 is available: https:\/\/github\.com\/koulaxizis\/oros-bridge\/releases\/tag\/v99\.0\.0/);
  release = { tag: "v99.0.0", files: {}, prerelease: true };
  r = await runAsync(exe, ["--check-update"], dir);
  assert.equal(r.status, 1);
  assert.match(r.err, /no usable release/);
  release = { tag: "v99.0", files: {} };
  r = await runAsync(exe, ["--check-update"], dir);
  assert.equal(r.status, 1);
});

test("update: the new program takes this copy's place, checked against SHA256SUMS.txt", { skip: !(IS_WIN || LINUX) }, async () => {
  const { dir, exe } = copy();
  const dll = path.join(dir, "oros-bridge-asio.dll");
  if (IS_WIN) fs.writeFileSync(dll, "old asio module");
  const before = fs.readFileSync(exe);
  release = newRelease("v99.1.0", dir);
  const r = await runAsync(exe, ["--update"], dir);
  assert.equal(r.status, 0, r.out + r.err);
  assert.match(r.out, /updated to 99\.1\.0/);
  assert.equal(fs.readFileSync(exe, "utf8"), expected("v99.1.0"));
  if (IS_WIN) {
    assert.equal(fs.readFileSync(dll, "utf8"), "new asio module v99.1.0");
    assert.ok(fs.readFileSync(exe + ".old").equals(before), "the old exe stays as .old until the next start");
    assert.equal(fs.readFileSync(dll + ".old", "utf8"), "old asio module");
  } else {
    assert.equal(fs.statSync(exe).mode & 0o777, 0o755);
  }
  const left = fs.readdirSync(dir).filter((n) => n.startsWith(".oros-bridge") || n.endsWith(".new"));
  assert.deepEqual(left, [], "no temporary files are left");
});

test("update: a file that does not match its checksum, or has none, is never used", { skip: !(IS_WIN || LINUX) }, async () => {
  const { dir, exe } = copy();
  const before = fs.readFileSync(exe);
  release = newRelease("v99.2.0", dir);
  release.sums = Object.keys(release.files).map((n) => `${"0".repeat(64)}  ${n}`).join("\n") + "\n";
  let r = await runAsync(exe, ["--update"], dir);
  assert.equal(r.status, 1);
  assert.match(r.err, /\(checksum\).*does not match/);
  assert.ok(fs.readFileSync(exe).equals(before), "the program is untouched");
  release.sums = "\n";
  r = await runAsync(exe, ["--update"], dir);
  assert.equal(r.status, 1);
  assert.match(r.err, /is not in SHA256SUMS\.txt/);
  assert.ok(fs.readFileSync(exe).equals(before));
  const left = fs.readdirSync(dir).filter((n) => n.startsWith(".oros-bridge") || n.endsWith(".new"));
  assert.deepEqual(left, []);
});

test("update: a copy orOS brought (--bundled, oros-bridge.bundled) leaves updates to orOS", async () => {
  const { dir, exe } = copy();
  const before = fs.readFileSync(exe);
  release = newRelease("v99.3.0", dir);
  let r = await runAsync(exe, ["--bundled", "--update"], dir);
  assert.equal(r.status, 1);
  assert.match(r.err, /\(manual\): orOS updates this copy/);
  fs.writeFileSync(path.join(dir, "oros-bridge.bundled"), "orOS Portable\n");
  r = await runAsync(exe, ["--update"], dir);
  assert.equal(r.status, 1);
  assert.match(r.err, /orOS updates this copy/);
  assert.ok(fs.readFileSync(exe).equals(before));
});

test("bundled: no window and no console, ready on the port, the code file works", async () => {
  const { dir, exe } = copy();
  const codeFile = path.join(dir, "code.txt");
  const env = { ...process.env, OROS_BRIDGE_UPDATE_URL: base };
  const p = spawn(exe, ["--bundled", "--port", "0", "--config-dir", path.join(dir, "cfg"), "--code-file", codeFile], { env });
  let out = "";
  p.stdout.on("data", (d) => (out += d));
  const end = Date.now() + 10000;
  while (!/ready on 127\.0\.0\.1:\d+/.test(out)) {
    if (Date.now() > end) { p.kill(); assert.fail("not ready: " + out); }
    await new Promise((r) => setTimeout(r, 50));
  }
  p.kill();
  assert.ok(!fs.existsSync(codeFile), "no code is open, so there is no code file");
});
