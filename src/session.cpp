#include "session.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include "base64.h"
#include "editor.h"
#include "mainloop.h"
#include "scan.h"

namespace ob {

Session::Session(Connection& conn, Config& cfg, Pairing& pairing) : conn_(conn), cfg_(cfg), pairing_(pairing) {
  nlohmann::json hello = {{"event", "hello"}, {"app", "oros-bridge"}, {"version", OROS_BRIDGE_VERSION},
                          {"editors", EditorWindow::supported()}};
  conn_.sendText(hello.dump());
}

Session::~Session() {
  meterQuit_ = true;
  if (meterThread_.joinable()) meterThread_.join();
  {
    std::lock_guard<std::mutex> g(audioMu_);
    audio_.close();
  }
  auto all = std::move(open_);
  MainLoop::get().call([&all] {
    for (auto& kv : all) {
      kv.second->plugin->closeEditor();
      kv.second->plugin.reset();
    }
    return 0;
  });
}

void Session::reply(const nlohmann::json& m, nlohmann::json out) {
  if (m.contains("id")) out["id"] = m["id"];
  conn_.sendText(out.dump());
}

void Session::onText(const std::string& text) {
  nlohmann::json m;
  try {
    m = nlohmann::json::parse(text);
  } catch (...) {
    conn_.sendText(R"({"ok":false,"error":"bad json"})");
    return;
  }
  if (!m.is_object() || !m.contains("op") || !m["op"].is_string()) {
    reply(m.is_object() ? m : nlohmann::json::object(), {{"ok", false}, {"error", "no op"}});
    return;
  }
  nlohmann::json out;
  try {
    out = command(m);
  } catch (const std::exception& e) {
    out = {{"ok", false}, {"error", std::string("bad request: ") + e.what()}};
  }
  reply(m, out);
}

static nlohmann::json fail(const std::string& e) { return {{"ok", false}, {"error", e}}; }

nlohmann::json Session::command(const nlohmann::json& m) {
  const std::string op = m["op"];
  // ----- before the page is known -----
  if (op == "pair") {
    if (!m.contains("code")) {
      pairing_.start();
      return {{"ok", true}, {"shown", true}};
    }
    auto token = pairing_.tryCode(m["code"].get<std::string>());
    if (token.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));   // slows down guessing
      return fail(pairing_.active() ? "wrong code" : "wrong code, ask for a new one");
    }
    authed_ = true;
    return {{"ok", true}, {"token", token}};
  }
  if (op == "auth") {
    authed_ = cfg_.hasToken(m.value("token", ""));
    if (!authed_) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return authed_ ? nlohmann::json{{"ok", true}} : fail("unknown token");
  }
  if (!authed_) return fail("not paired");

  // ----- plugins on this computer -----
  if (op == "list") return {{"ok", true}, {"plugins", cfg_.plugins()}};
  if (op == "scan") {
    auto list = scanPlugins(cfg_, m.value("full", false), [this](const std::string& path) {
      conn_.sendText(nlohmann::json{{"event", "scanning"}, {"path", path}}.dump());
    });
    return {{"ok", true}, {"plugins", list}};
  }
  if (op == "folders") return {{"ok", true}, {"defaults", Config::defaultFolders()}, {"extra", cfg_.extraFolders()}};
  if (op == "setFolders") {
    std::vector<std::string> f;
    if (!m.contains("extra") || !m["extra"].is_array()) return fail("bad folders");
    for (auto& x : m["extra"]) {
      if (!x.is_string()) return fail("bad folder");
      auto s = x.get<std::string>();
      if (s.empty() || s.size() > 1024) return fail("bad folder");
      f.push_back(s);
    }
    if (f.size() > 32) return fail("too many folders");
    cfg_.setExtraFolders(f);
    return {{"ok", true}};
  }

  // ----- the sound card -----
  if (op.rfind("audio", 0) == 0 || op == "takeRead") return audioCommand(op, m);
  if (op.rfind("live", 0) == 0 || op.rfind("midi", 0) == 0) return liveCommand(op, m);

  // ----- open plugins -----
  if (op == "open") {
    if (open_.size() >= kMaxOpen) return fail("too many open plugins");
    PluginInfo info;
    if (!cfg_.find(m.value("plugin", ""), info)) return fail("unknown plugin");
    double sr = m.value("sr", 48000.0);
    uint32_t block = m.value("block", 1024u);
    if (sr < 8000 || sr > 384000 || block < 16 || block > 16384) return fail("bad sample rate or block size");
    std::string err;
    auto o = std::make_shared<Open>();
    o->info = info;
    o->maxFrames = block;
    bool ok = MainLoop::get().call([&] {
      o->plugin = openPlugin(info, err);
      if (!o->plugin) return false;
      if (!o->plugin->activate(sr, block, false, err)) { o->plugin.reset(); return false; }
      return true;
    });
    if (!ok) return fail(err.empty() ? "could not open" : err);
    int h = nextHandle_++;
    open_[h] = o;
    return MainLoop::get().call([&] {
      return nlohmann::json{{"ok", true}, {"h", h}, {"inputs", o->plugin->inputs()}, {"outputs", o->plugin->outputs()},
                            {"latency", o->plugin->latency()}, {"tail", o->plugin->tail()},
                            {"editor", o->plugin->hasEditor()}, {"instrument", info.instrument}};
    });
  }
  int h = m.value("h", 0);
  auto it = open_.find(h);
  if (it == open_.end()) return fail("unknown handle");
  auto o = it->second;
  if (op == "close" && o->fx) return liveCommand("liveClose", m);
  if (op == "reset" && o->fx) return fail("live: the plugin runs on the sound card");
  if (op == "close") {
    open_.erase(it);
    MainLoop::get().call([&] { o->plugin->closeEditor(); o->plugin.reset(); return 0; });
    return {{"ok", true}};
  }
  if (op == "info") {
    return MainLoop::get().call([&] {
      return nlohmann::json{{"ok", true}, {"latency", o->plugin->latency()}, {"tail", o->plugin->tail()},
                            {"dirty", o->plugin->dirty()}};
    });
  }
  if (op == "reset") {
    MainLoop::get().call([&] { o->plugin->reset(); return 0; });
    return {{"ok", true}};
  }
  if (op == "getState") {
    std::vector<uint8_t> st;
    bool ok = MainLoop::get().call([&] { return o->plugin->getState(st); });
    if (!ok) return fail("the plugin did not give its state");
    return {{"ok", true}, {"data", base64Encode(st)}};
  }
  if (op == "setState") {
    std::vector<uint8_t> st;
    if (!base64Decode(m.value("data", ""), st)) return fail("bad state");
    bool ok = MainLoop::get().call([&] { return o->plugin->setState(st); });
    return ok ? nlohmann::json{{"ok", true}} : fail("the plugin refused the state");
  }
  if (op == "editor") {
    std::string err;
    auto title = o->info.name + " - orOS Studio";
    bool ok = MainLoop::get().call([&] {
      return o->plugin->openEditor(title, [this, h] {
        conn_.sendText(nlohmann::json{{"event", "editorClosed"}, {"h", h}}.dump());
      }, err);
    });
    return ok ? nlohmann::json{{"ok", true}} : fail(err);
  }
  if (op == "closeEditor") {
    MainLoop::get().call([&] { o->plugin->closeEditor(); return 0; });
    return {{"ok", true}};
  }
  return fail("unknown op");
}

// The sound card: list, open with the inputs to record, the sound to play
// (sent ahead in "ORP1" blocks), start, stop, read the take back.
nlohmann::json Session::audioCommand(const std::string& op, const nlohmann::json& m) {
  if (op == "audioDevices") {
    auto list = nlohmann::json::array();
    for (auto* b : audioBackends()) {
      nlohmann::json d, e = {{"id", b->id()}, {"label", b->label()}, {"exclusive", b->canExclusive()}};
      std::string err;
      if (b->devices(d, err)) { e["ok"] = true; e["inputs"] = d["inputs"]; e["outputs"] = d["outputs"]; }
      else { e["ok"] = false; e["error"] = err; }
      list.push_back(e);
    }
    return {{"ok", true}, {"backends", list}};
  }
  std::lock_guard<std::mutex> g(audioMu_);
  if (op == "audioOpen") {
    auto* b = findAudioBackend(m.value("backend", ""));
    if (!b) return fail("unknown backend");
    StreamSpec spec;
    spec.input = m.value("input", "");
    spec.output = m.value("output", "");
    spec.sampleRate = m.value("sr", 0u);
    spec.period = m.value("period", 256u);
    spec.exclusive = m.value("exclusive", false);
    if ((spec.sampleRate && (spec.sampleRate < 8000 || spec.sampleRate > 384000)) || spec.period < 16 || spec.period > 8192)
      return fail("bad sample rate or period");
    std::vector<uint32_t> rec;
    if (m.contains("record")) {
      if (!m["record"].is_array() || m["record"].size() > kMaxRecordChannels) return fail("bad inputs");
      for (auto& x : m["record"]) {
        if (!x.is_number_unsigned() || x.get<uint32_t>() >= 1024) return fail("bad inputs");
        rec.push_back(x.get<uint32_t>());
      }
    }
    std::string err;
    closeLive();
    if (!audio_.open(*b, spec, rec, err)) return fail(err);
    if (!meterThread_.joinable()) meterThread_ = std::thread([this] { meters(); });
    auto& s = audio_.stream();
    return {{"ok", true}, {"sr", s.sampleRate}, {"period", s.period}, {"inputs", s.inChannels},
            {"outputs", s.outChannels}, {"latency", s.latency}, {"record", rec}};
  }
  if (op == "audioClose") {
    closeLive();
    audio_.close();
    return {{"ok", true}};
  }
  if (!audio_.isOpen()) return fail("no sound card open");
  std::string err;
  if (op == "audioPlay") {
    if (!audio_.prepare(m.value("frames", 0u), m.value("channels", 0u), m.value("out", 0u), err)) return fail(err);
    return {{"ok", true}};
  }
  if (op == "audioStart") {
    if (!audio_.start(err)) return fail(err);
    return {{"ok", true}};
  }
  if (op == "audioStop") {
    uint64_t frames = 0, lost = 0;
    if (!audio_.stop(frames, lost, err)) return fail(err);
    return {{"ok", true}, {"frames", frames}, {"lost", lost}};
  }
  if (op == "takeRead") {
    uint64_t offset = m.value("offset", uint64_t(0));
    uint32_t frames = std::min<uint32_t>(m.value("frames", 65536u), 1u << 18);
    std::vector<float> planar;
    uint32_t got = 0;
    if (!audio_.readTake(offset, frames, planar, got, err)) return fail(err);
    // "ORT1" | u32 offset | u32 frames | u16 channels | u16 0 | planar float32
    uint32_t nch = uint32_t(audio_.recordChannels().size());
    std::vector<uint8_t> out(16 + planar.size() * 4);
    std::memcpy(out.data(), "ORT1", 4);
    for (int i = 0; i < 4; i++) { out[4 + i] = uint8_t(uint32_t(offset) >> (8 * i)); out[8 + i] = uint8_t(got >> (8 * i)); }
    out[12] = uint8_t(nch);
    out[13] = uint8_t(nch >> 8);
    if (!planar.empty()) std::memcpy(out.data() + 16, planar.data(), planar.size() * 4);
    conn_.sendBinary(out);
    return {{"ok", true}, {"frames", got}};
  }
  return fail("unknown op");
}

// A plugin live on the card's inputs: the person hears themselves through it.
// A second instance of the plugin, activated for real time at the card's rate;
// the page gives it the state of the track's plugin. Only while stopped.
nlohmann::json Session::liveCommand(const std::string& op, const nlohmann::json& m) {
  std::lock_guard<std::mutex> g(audioMu_);
  if (op == "liveClose") {
    closeLive();
    return {{"ok", true}};
  }
  if (op == "midiDevices") return {{"ok", true}, {"inputs", midiInputs()}};
  auto it = open_.find(liveH_);
  if (op == "midiTake") {
    std::vector<AudioEngine::MidiTaken> ev;
    uint64_t lost = 0;
    std::string err;
    if (!audio_.readMidiTake(ev, lost, err)) return fail(err);
    auto list = nlohmann::json::array();
    for (auto& e : ev) list.push_back({e.frame, e.status, e.d1, e.d2});
    return {{"ok", true}, {"events", list}, {"lost", lost}};
  }
  if (op == "midiTest") {   // the test keyboard (--test-audio): bytes as if played
    if (it == open_.end() || !it->second->midi) return fail("no live keyboard");
    std::vector<uint8_t> b;
    for (auto& x : m.value("bytes", nlohmann::json::array())) b.push_back(uint8_t(x.get<uint32_t>()));
    return it->second->midi->inject(b) ? nlohmann::json{{"ok", true}} : fail("not the test keyboard");
  }
  if (op == "liveSet") {
    if (it == open_.end()) return fail("no live plugin");
    auto& fx = *it->second->fx;
    if (m.contains("gain")) {
      double gn = m["gain"].get<double>();
      if (!(gn >= 0 && gn <= 4)) return fail("bad gain");
      fx.gain = float(gn);
    }
    if (m.contains("on")) fx.on = m["on"].get<bool>();
    return {{"ok", true}, {"on", fx.on.load()}, {"gain", fx.gain.load()}, {"slow", fx.slow.load()}, {"failed", fx.failed.load()}};
  }
  if (op != "liveOpen") return fail("unknown op");
  if (!audio_.isOpen()) return fail("no sound card open");
  if (audio_.running()) return fail("playing");
  const auto& st = audio_.stream();
  PluginInfo info;
  if (!cfg_.find(m.value("plugin", ""), info)) return fail("unknown plugin");
  std::string midiName = m.value("midi", std::string());
  if (info.instrument && midiName.empty()) return fail("an instrument plays live from a MIDI keyboard: choose one");
  if (open_.size() >= kMaxOpen) return fail("too many open plugins");
  std::vector<uint32_t> inputs;
  auto jin = m.value("inputs", nlohmann::json::array());
  if (!jin.is_array() || jin.size() > 2 || (jin.empty() && !info.instrument)) return fail("bad inputs");
  for (auto& x : jin) {
    if (!x.is_number_unsigned() || x.get<uint32_t>() >= st.inChannels) return fail("bad inputs");
    inputs.push_back(x.get<uint32_t>());
  }
  uint32_t out = m.value("out", 0u);
  if (out >= st.outChannels) return fail("bad output");
  double gain = m.value("gain", 1.0);
  if (!(gain >= 0 && gain <= 4)) return fail("bad gain");
  std::string rec = m.value("record", std::string("dry"));
  if (rec != "dry" && rec != "wet") return fail("bad record");
  std::vector<uint8_t> state;
  if (m.contains("state") && !base64Decode(m.value("state", ""), state)) return fail("bad state");
  closeLive();
  std::unique_ptr<MidiInput> midiIn;
  if (!midiName.empty()) {
    std::string merr;
    midiIn = openMidiInput(midiName, merr);
    if (!midiIn) return fail(merr);
  }
  const uint32_t maxFrames = std::min<uint32_t>(std::max<uint32_t>(st.period, 512), 8192);
  auto o = std::make_shared<Open>();
  o->info = info;
  o->maxFrames = maxFrames;
  std::string err;
  bool ok = MainLoop::get().call([&] {
    o->plugin = openPlugin(info, err);
    if (!o->plugin) return false;
    if (!state.empty() && !o->plugin->setState(state)) { err = "the plugin refused the state"; o->plugin.reset(); return false; }
    if (!o->plugin->activate(double(st.sampleRate), maxFrames, true, err)) { o->plugin.reset(); return false; }
    if (!o->plugin->inputs() && !info.instrument) { err = "the plugin has no audio input"; o->plugin.reset(); return false; }
    return true;
  });
  if (!ok) return fail(err.empty() ? "could not open" : err);
  auto fx = std::make_unique<LiveFx>();
  fx->plugin = o->plugin.get();
  fx->midi = midiIn.get();
  o->midi = std::move(midiIn);
  fx->inputs = info.instrument ? std::vector<uint32_t>() : inputs;
  fx->out = out;
  fx->wet = rec == "wet";
  fx->gain = float(gain);
  fx->on = m.value("on", false);
  nlohmann::json res = MainLoop::get().call([&] {
    fx->prepare(maxFrames, o->plugin->inputs(), o->plugin->outputs());
    return nlohmann::json{{"latency", o->plugin->latency()}, {"inputs", o->plugin->inputs()}, {"outputs", o->plugin->outputs()},
                          {"editor", o->plugin->hasEditor()}};
  });
  o->fx = std::move(fx);
  if (!audio_.setLive(o->fx.get(), err)) {
    MainLoop::get().call([&] { o->plugin.reset(); return 0; });
    return fail(err);
  }
  int h = nextHandle_++;
  open_[h] = o;
  liveH_ = h;
  res["ok"] = true;
  res["h"] = h;
  res["monitor"] = st.latency + res["latency"].get<uint32_t>();   // what the person hears, input to output
  res["on"] = o->fx->on.load();
  res["record"] = rec;
  res["instrument"] = info.instrument;
  res["midi"] = midiName;
  return res;
}

void Session::closeLive() {
  if (!liveH_) return;
  std::string err;
  audio_.setLive(nullptr, err);   // when stopped; while playing the card keeps it until close
  auto it = open_.find(liveH_);
  liveH_ = 0;
  if (it == open_.end()) return;
  auto o = it->second;
  if (audio_.running()) {
    // never pull a plugin from under a running take: stop first
    uint64_t f = 0, l = 0;
    audio_.stop(f, l, err);
    audio_.setLive(nullptr, err);
  }
  open_.erase(it);
  MainLoop::get().call([&] { o->plugin->closeEditor(); o->plugin.reset(); return 0; });
}

// Every 100 ms while a card is open: the loudest sample of each input and
// where the take is, so Studio can show meters before and during a take.
void Session::meters() {
  while (!meterQuit_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    nlohmann::json ev;
    {
      std::lock_guard<std::mutex> g(audioMu_);
      if (!audio_.isOpen()) continue;
      ev = {{"event", "levels"}, {"peaks", audio_.takePeaks()}, {"running", audio_.running()}, {"pos", audio_.position()}};
      if (auto* fx = audio_.live())
        ev["live"] = {{"h", liveH_}, {"on", fx->on.load()}, {"peak", fx->peak.exchange(0.f)}, {"slow", fx->slow.load()},
                      {"failed", fx->failed.load()}};
    }
    conn_.sendText(ev.dump());
  }
}

// Sound to play: "ORP1" | u32 offset | u32 frames | u16 channels | u16 0 | planar float32
void Session::playBlock(const std::vector<uint8_t>& d) {
  auto u32 = [&](size_t at) { return uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24; };
  uint32_t offset = u32(4), frames = u32(8), nch = uint32_t(d[12]) | uint32_t(d[13]) << 8;
  std::string err;
  if (d.size() != 16 + size_t(nch) * frames * 4) err = "bad block size";
  else {
    std::lock_guard<std::mutex> g(audioMu_);
    if (!audio_.isOpen()) err = "no sound card open";
    else audio_.fill(offset, frames, nch, d.data() + 16, err);
  }
  if (!err.empty()) conn_.sendText(nlohmann::json{{"event", "audioError"}, {"offset", offset}, {"error", err}}.dump());
}

// A block to render: "ORB1", u32 handle, u32 seq, u32 frames, u16 channels,
// u16 events, events (u32 frame, status, data1, data2, 0), float32 planar.
// The answer has the same header with the output channels and no events.
void Session::onBinary(const std::vector<uint8_t>& d) {
  auto u32 = [&](size_t at) { return uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24; };
  auto u16 = [&](size_t at) { return uint32_t(d[at]) | uint32_t(d[at + 1]) << 8; };
  auto error = [&](uint32_t h, uint32_t seq, const std::string& e) {
    conn_.sendText(nlohmann::json{{"event", "renderError"}, {"h", h}, {"seq", seq}, {"error", e}}.dump());
  };
  if (!authed_) { error(0, 0, "not paired"); return; }
  if (d.size() >= 16 && std::memcmp(d.data(), "ORP1", 4) == 0) { playBlock(d); return; }
  if (d.size() < 20 || std::memcmp(d.data(), "ORB1", 4) != 0) { error(0, 0, "bad block"); return; }
  uint32_t h = u32(4), seq = u32(8), frames = u32(12), nch = u16(16), nev = u16(18);
  auto it = open_.find(int(h));
  if (it == open_.end()) { error(h, seq, "unknown handle"); return; }
  auto o = it->second;
  if (o->fx) { error(h, seq, "live: the plugin runs on the sound card"); return; }
  size_t need = 20 + size_t(nev) * 8 + size_t(nch) * frames * 4;
  if (!frames || frames > o->maxFrames || nch > kMaxChannels || d.size() != need) { error(h, seq, "bad block size"); return; }
  std::vector<MidiEvent> events;
  for (uint32_t i = 0; i < nev; i++) {
    size_t at = 20 + size_t(i) * 8;
    events.push_back({u32(at), d[at + 4], uint8_t(d[at + 5] & 127), uint8_t(d[at + 6] & 127)});
  }
  const uint8_t* audio = d.data() + 20 + size_t(nev) * 8;
  std::vector<uint8_t> out;
  bool ok = MainLoop::get().call([&] {
    auto& p = *o->plugin;
    uint32_t ins = p.inputs(), outs = p.outputs();
    std::vector<std::vector<float>> inBuf(ins, std::vector<float>(frames, 0.f));
    for (uint32_t c = 0; c < ins && nch; c++) {
      uint32_t from = std::min(c, nch - 1);   // mono in: the same on both sides
      std::memcpy(inBuf[c].data(), audio + size_t(from) * frames * 4, size_t(frames) * 4);
    }
    std::vector<const float*> inPtr;
    for (auto& b : inBuf) inPtr.push_back(b.data());
    out.assign(20 + size_t(outs) * frames * 4, 0);
    std::vector<float*> outPtr;
    for (uint32_t c = 0; c < outs; c++) outPtr.push_back(reinterpret_cast<float*>(out.data() + 20 + size_t(c) * frames * 4));
    // the output buffer may not be float-aligned in a vector<uint8_t>: render into floats, then copy
    std::vector<std::vector<float>> tmp(outs, std::vector<float>(frames, 0.f));
    for (uint32_t c = 0; c < outs; c++) outPtr[c] = tmp[c].data();
    if (!p.process(inPtr.data(), outPtr.data(), frames, events)) return false;
    for (uint32_t c = 0; c < outs; c++) std::memcpy(out.data() + 20 + size_t(c) * frames * 4, tmp[c].data(), size_t(frames) * 4);
    std::memcpy(out.data(), "ORB1", 4);
    for (int i = 0; i < 4; i++) { out[4 + i] = uint8_t(h >> (8 * i)); out[8 + i] = uint8_t(seq >> (8 * i)); out[12 + i] = uint8_t(frames >> (8 * i)); }
    out[16] = uint8_t(outs);
    out[17] = 0;
    return true;
  });
  if (!ok) { error(h, seq, "the plugin failed"); return; }
  conn_.sendBinary(out);
}
}
