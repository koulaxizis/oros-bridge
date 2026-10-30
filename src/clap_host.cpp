// Hosting CLAP plugins (free-audio/clap 1.2.9).
#include <algorithm>
#include <atomic>
#include <cstring>
#include <set>
#include <clap/clap.h>
#include "dynlib.h"
#include "editor.h"
#include "mainloop.h"
#include "plugin.h"

namespace ob {
namespace {

// One loaded .clap file; shared by the plugins made from it.
struct ClapModule {
  std::unique_ptr<DynLib> lib;
  const clap_plugin_entry_t* entry = nullptr;
  const clap_plugin_factory_t* factory = nullptr;
  ~ClapModule() {
    if (entry) entry->deinit();
  }
};

std::shared_ptr<ClapModule> loadModule(const std::string& path, std::string& err) {
  auto m = std::make_shared<ClapModule>();
  m->lib = std::make_unique<DynLib>(path);
  if (!m->lib->ok()) { err = "cannot load: " + DynLib::lastError(); return nullptr; }
  m->entry = static_cast<const clap_plugin_entry_t*>(m->lib->symbol("clap_entry"));
  if (!m->entry) { err = "no clap_entry"; return nullptr; }
  if (!clap_version_is_compatible(m->entry->clap_version)) { err = "CLAP version not supported"; m->entry = nullptr; return nullptr; }
  if (!m->entry->init(path.c_str())) { err = "clap_entry init failed"; m->entry = nullptr; return nullptr; }
  m->factory = static_cast<const clap_plugin_factory_t*>(m->entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
  if (!m->factory) { err = "no plugin factory"; return nullptr; }
  return m;
}

bool hasFeature(const clap_plugin_descriptor_t* d, const char* f) {
  if (!d->features) return false;
  for (auto p = d->features; *p; ++p)
    if (std::strcmp(*p, f) == 0) return true;
  return false;
}

// The events of one block, CLAP style.
struct EventList {
  std::vector<std::vector<uint8_t>> storage;
  std::vector<const clap_event_header_t*> list;
  clap_input_events_t in{this, &EventList::size, &EventList::get};
  static uint32_t size(const clap_input_events_t* l) { return uint32_t(static_cast<const EventList*>(l->ctx)->list.size()); }
  static const clap_event_header_t* get(const clap_input_events_t* l, uint32_t i) {
    auto* self = static_cast<const EventList*>(l->ctx);
    return i < self->list.size() ? self->list[i] : nullptr;
  }
  template <class T>
  void push(const T& ev) {
    storage.emplace_back(sizeof(T));
    std::memcpy(storage.back().data(), &ev, sizeof(T));
  }
  void finish() {
    list.clear();
    for (auto& s : storage) list.push_back(reinterpret_cast<const clap_event_header_t*>(s.data()));
    std::stable_sort(list.begin(), list.end(), [](auto a, auto b) { return a->time < b->time; });
  }
  void clear() { storage.clear(); list.clear(); }
};

const clap_output_events_t kDropEvents{nullptr, [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};

class ClapPlugin;
// plugins still alive: work posted for later checks here first (all on the main thread)
std::set<ClapPlugin*>& alive() {
  static std::set<ClapPlugin*> s;
  return s;
}
void later(ClapPlugin* p, std::function<void(ClapPlugin*)> f) {
  MainLoop::get().post([p, f] { if (alive().count(p)) f(p); });
}

class ClapPlugin : public Plugin {
public:
  ClapPlugin() { MainLoop::get().call([this] { alive().insert(this); }); }
  std::shared_ptr<ClapModule> mod;
  const clap_plugin_t* p = nullptr;
  clap_host_t host{};
  std::string name;
  const clap_plugin_audio_ports_t* ports = nullptr;
  const clap_plugin_state_t* state = nullptr;
  const clap_plugin_latency_t* lat = nullptr;
  const clap_plugin_tail_t* tailExt = nullptr;
  const clap_plugin_gui_t* gui = nullptr;
  const clap_plugin_params_t* params = nullptr;
  const clap_plugin_note_ports_t* notePorts = nullptr;
  uint32_t ins = 0, outs = 2, maxFrames = 0;
  bool active = false, processing = false, guiOpen = false;
  std::atomic<bool> dirtyFlag{false}, flushWanted{false};
  int64_t steady = 0;
  std::unique_ptr<EditorWindow> win;
  std::function<void()> onEditorClosed;
  // extra ports get silence / a scratch buffer
  std::vector<std::vector<float>> scratch;

  ~ClapPlugin() override {
    alive().erase(this);
    closeEditor();
    if (p) {
      if (processing) p->stop_processing(p);
      if (active) p->deactivate(p);
      p->destroy(p);
    }
  }

  static ClapPlugin* self(const clap_host_t* h) { return static_cast<ClapPlugin*>(h->host_data); }

  // what the bridge offers a plugin
  static const void* hostExtension(const clap_host_t* h, const char* id) {
    static const clap_host_state_t hostState{[](const clap_host_t* h) { self(h)->dirtyFlag = true; }};
    static const clap_host_latency_t hostLatency{[](const clap_host_t*) {}};
    static const clap_host_tail_t hostTail{[](const clap_host_t*) {}};
    static const clap_host_params_t hostParams{
        [](const clap_host_t* h, clap_param_rescan_flags) { self(h)->dirtyFlag = true; },
        [](const clap_host_t*, clap_id, clap_param_clear_flags) {},
        [](const clap_host_t* h) {
          auto* s = self(h);
          s->dirtyFlag = true;
          if (!s->flushWanted.exchange(true)) later(s, [](ClapPlugin* x) { x->flushParams(); });
        }};
    static const clap_host_audio_ports_t hostPorts{[](const clap_host_t*, uint32_t) { return false; },
                                                   [](const clap_host_t*, uint32_t) {}};
    static const clap_host_note_ports_t hostNotePorts{
        [](const clap_host_t*) -> uint32_t { return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; },
        [](const clap_host_t*, uint32_t) {}};
    static const clap_host_gui_t hostGui{
        [](const clap_host_t*) {},
        [](const clap_host_t* h, uint32_t w, uint32_t hh) {
          auto* s = self(h);
          later(s, [w, hh](ClapPlugin* x) { if (x->win) x->win->resize(int(w), int(hh)); });
          return true;
        },
        [](const clap_host_t*) { return false; },
        [](const clap_host_t*) { return false; },
        [](const clap_host_t* h, bool) {
          later(self(h), [](ClapPlugin* x) { if (x->guiOpen) x->editorClosedByPerson(); });
        }};
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &hostState;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &hostLatency;
    if (!std::strcmp(id, CLAP_EXT_TAIL)) return &hostTail;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &hostParams;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &hostPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &hostNotePorts;
    if (!std::strcmp(id, CLAP_EXT_GUI)) return &hostGui;
    return nullptr;
  }

  bool load(const PluginInfo& info, std::string& err) {
    mod = loadModule(info.path, err);
    if (!mod) return false;
    std::string pid = info.id.substr(5);   // after "clap:"
    host.clap_version = CLAP_VERSION;
    host.host_data = this;
    host.name = "orOS Bridge";
    host.vendor = "orOS";
    host.url = "https://useoros.online";
    host.version = OROS_BRIDGE_VERSION;
    host.get_extension = &ClapPlugin::hostExtension;
    host.request_restart = [](const clap_host_t*) {};
    host.request_process = [](const clap_host_t*) {};
    host.request_callback = [](const clap_host_t* h) {
      auto* s = self(h);
      later(s, [](ClapPlugin* x) { if (x->p) x->p->on_main_thread(x->p); });
    };
    p = mod->factory->create_plugin(mod->factory, &host, pid.c_str());
    if (!p) { err = "the plugin could not be created"; return false; }
    if (!p->init(p)) { p->destroy(p); p = nullptr; err = "the plugin did not start"; return false; }
    name = info.name;
    ports = static_cast<const clap_plugin_audio_ports_t*>(p->get_extension(p, CLAP_EXT_AUDIO_PORTS));
    state = static_cast<const clap_plugin_state_t*>(p->get_extension(p, CLAP_EXT_STATE));
    lat = static_cast<const clap_plugin_latency_t*>(p->get_extension(p, CLAP_EXT_LATENCY));
    tailExt = static_cast<const clap_plugin_tail_t*>(p->get_extension(p, CLAP_EXT_TAIL));
    gui = static_cast<const clap_plugin_gui_t*>(p->get_extension(p, CLAP_EXT_GUI));
    params = static_cast<const clap_plugin_params_t*>(p->get_extension(p, CLAP_EXT_PARAMS));
    notePorts = static_cast<const clap_plugin_note_ports_t*>(p->get_extension(p, CLAP_EXT_NOTE_PORTS));
    ins = mainChannels(true);
    outs = mainChannels(false);
    if (!outs) { err = "the plugin has no audio output"; return false; }
    return true;
  }

  uint32_t mainChannels(bool input) {
    if (!ports) return input ? 0 : 2;   // no audio-ports: stereo out only
    if (!ports->count(p, input)) return 0;
    clap_audio_port_info_t info{};
    if (!ports->get(p, 0, input, &info)) return 0;
    return std::min<uint32_t>(info.channel_count, kMaxChannels);
  }

  void flushParams() {
    flushWanted = false;
    if (!params || !p || processing) return;
    EventList none;
    none.finish();
    params->flush(p, &none.in, &kDropEvents);
  }

  bool activate(double sr, uint32_t frames, std::string& err) override {
    if (processing) { p->stop_processing(p); processing = false; }
    if (active) { p->deactivate(p); active = false; }
    if (!p->activate(p, sr, 1, frames)) { err = "the plugin refused the sample rate or block size"; return false; }
    active = true;
    maxFrames = frames;
    scratch.assign(64, std::vector<float>(frames, 0.f));
    if (!p->start_processing(p)) { err = "the plugin did not start processing"; return false; }
    processing = true;
    steady = 0;
    return true;
  }
  uint32_t inputs() const override { return ins; }
  uint32_t outputs() const override { return outs; }
  uint32_t latency() override { return lat && active ? lat->get(p) : 0; }
  uint32_t tail() override {
    if (!tailExt || !active) return 0;
    uint32_t t = tailExt->get(p);
    return std::min<uint32_t>(t, kMaxTail);
  }

  bool process(const float* const* in, float* const* out, uint32_t frames, const std::vector<MidiEvent>& events) override {
    if (!processing || frames > maxFrames) return false;
    EventList ev;
    for (auto& m : events) {
      uint32_t t = std::min(m.offset, frames - 1);
      uint8_t kind = m.status & 0xF0, ch = m.status & 0x0F;
      if (kind == 0x90 || kind == 0x80) {
        clap_event_note_t n{};
        bool on = kind == 0x90 && m.d2 > 0;
        n.header = {sizeof(n), t, CLAP_CORE_EVENT_SPACE_ID, uint16_t(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
        n.note_id = -1;
        n.port_index = 0;
        n.channel = ch;
        n.key = m.d1 & 127;
        n.velocity = (on || kind == 0x80) ? m.d2 / 127.0 : 0.0;
        ev.push(n);
      } else {
        clap_event_midi_t x{};
        x.header = {sizeof(x), t, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
        x.port_index = 0;
        x.data[0] = m.status; x.data[1] = m.d1; x.data[2] = m.d2;
        ev.push(x);
      }
    }
    ev.finish();
    // main ports carry the audio, any other port gets silence in, scratch out
    uint32_t nIn = ports ? ports->count(p, true) : 0, nOut = ports ? ports->count(p, false) : 1;
    std::vector<clap_audio_buffer_t> bin(nIn), bout(std::max<uint32_t>(nOut, 1));
    std::vector<std::vector<float*>> pin(nIn), pout(bout.size());
    size_t s = 0;
    auto take = [&](bool silent) {
      if (s >= scratch.size()) scratch.emplace_back(maxFrames, 0.f);
      auto* b = scratch[s++].data();
      if (silent) std::fill(b, b + frames, 0.f);
      return b;
    };
    for (uint32_t i = 0; i < nIn; i++) {
      clap_audio_port_info_t info{};
      uint32_t ch = ports->get(p, i, true, &info) ? info.channel_count : 0;
      for (uint32_t c = 0; c < ch; c++)
        pin[i].push_back(i == 0 && c < ins ? const_cast<float*>(in[c]) : take(true));
      bin[i] = {pin[i].data(), nullptr, uint32_t(pin[i].size()), 0, 0};
    }
    for (uint32_t i = 0; i < bout.size(); i++) {
      uint32_t ch = 2;
      if (ports) {
        clap_audio_port_info_t info{};
        ch = ports->get(p, i, false, &info) ? info.channel_count : 0;
      }
      for (uint32_t c = 0; c < ch; c++) pout[i].push_back(i == 0 && c < outs ? out[c] : take(false));
      bout[i] = {pout[i].data(), nullptr, uint32_t(pout[i].size()), 0, 0};
    }
    clap_process_t pr{};
    pr.steady_time = steady;
    pr.frames_count = frames;
    pr.transport = nullptr;
    pr.audio_inputs = bin.data();
    pr.audio_outputs = bout.data();
    pr.audio_inputs_count = nIn;
    pr.audio_outputs_count = uint32_t(bout.size());
    pr.in_events = &ev.in;
    pr.out_events = &kDropEvents;
    auto status = p->process(p, &pr);
    steady += frames;
    if (status == CLAP_PROCESS_ERROR) return false;
    if (status == CLAP_PROCESS_SLEEP || status == CLAP_PROCESS_TAIL) {
      // the plugin may leave the output alone when it is quiet: clear what it did not mark constant
    }
    return true;
  }

  void reset() override {
    if (active) p->reset(p);
    steady = 0;
  }

  bool getState(std::vector<uint8_t>& out) override {
    out.clear();
    if (!state) return true;   // no state: nothing to keep
    clap_ostream_t os{&out, [](const clap_ostream_t* s, const void* buf, uint64_t size) -> int64_t {
                        auto* v = static_cast<std::vector<uint8_t>*>(s->ctx);
                        if (v->size() + size > (64u << 20)) return -1;
                        auto* b = static_cast<const uint8_t*>(buf);
                        v->insert(v->end(), b, b + size);
                        return int64_t(size);
                      }};
    if (!state->save(p, &os)) return false;
    dirtyFlag = false;
    return true;
  }
  bool setState(const std::vector<uint8_t>& in) override {
    if (!state) return in.empty();
    struct Reader { const std::vector<uint8_t>* v; size_t pos; } r{&in, 0};
    clap_istream_t is{&r, [](const clap_istream_t* s, void* buf, uint64_t size) -> int64_t {
                        auto* r = static_cast<Reader*>(s->ctx);
                        uint64_t n = std::min<uint64_t>(size, r->v->size() - r->pos);
                        std::memcpy(buf, r->v->data() + r->pos, size_t(n));
                        r->pos += size_t(n);
                        return int64_t(n);
                      }};
    bool ok = state->load(p, &is);
    dirtyFlag = false;
    return ok;
  }

  bool hasEditor() override {
#ifdef _WIN32
    return gui && gui->is_api_supported(p, CLAP_WINDOW_API_WIN32, false);
#else
    return false;
#endif
  }
  bool openEditor(const std::string& title, std::function<void()> onClosed, std::string& err) override {
    if (guiOpen) { if (win) win->show(); return true; }
    if (!hasEditor()) { err = "this plugin has no window here"; return false; }
#ifdef _WIN32
    if (!gui->create(p, CLAP_WINDOW_API_WIN32, false)) { err = "the plugin window could not be made"; return false; }
    guiOpen = true;
    uint32_t w = 400, h = 300;
    gui->get_size(p, &w, &h);
    bool resizable = gui->can_resize(p);
    onEditorClosed = std::move(onClosed);
    win = EditorWindow::create(title, int(w), int(h), resizable, [this] { editorClosedByPerson(); },
                               [this](int nw, int nh) {
                                 if (!guiOpen) return;
                                 uint32_t aw = uint32_t(nw), ah = uint32_t(nh);
                                 if (gui->adjust_size(p, &aw, &ah)) gui->set_size(p, aw, ah);
                               });
    if (!win) { closeEditor(); err = "no window"; return false; }
    clap_window_t cw{};
    cw.api = CLAP_WINDOW_API_WIN32;
    cw.win32 = win->nativeHandle();
    if (!gui->set_parent(p, &cw)) { closeEditor(); err = "the plugin window could not be placed"; return false; }
    gui->show(p);
    win->show();
    return true;
#else
    (void)title; (void)onClosed;
    err = "plugin windows are Windows only for now";
    return false;
#endif
  }
  void editorClosedByPerson() {
    auto cb = onEditorClosed;
    closeEditor();
    if (cb) cb();
  }
  void closeEditor() override {
    if (guiOpen && gui) gui->destroy(p);
    guiOpen = false;
    win.reset();
    onEditorClosed = nullptr;
  }
  bool dirty() override { return dirtyFlag; }
};
}

std::unique_ptr<Plugin> openClap(const PluginInfo& info, std::string& err) {
  auto pl = std::make_unique<ClapPlugin>();
  if (!pl->load(info, err)) return nullptr;
  return pl;
}

std::vector<PluginInfo> describeClap(const std::string& path, std::string& err) {
  std::vector<PluginInfo> out;
  auto m = loadModule(path, err);
  if (!m) return out;
  uint32_t n = m->factory->get_plugin_count(m->factory);
  for (uint32_t i = 0; i < n; i++) {
    auto* d = m->factory->get_plugin_descriptor(m->factory, i);
    if (!d || !d->id || !d->name) continue;
    PluginInfo info;
    info.id = std::string("clap:") + d->id;
    info.format = "clap";
    info.name = d->name;
    info.vendor = d->vendor ? d->vendor : "";
    info.version = d->version ? d->version : "";
    info.path = path;
    info.instrument = hasFeature(d, CLAP_PLUGIN_FEATURE_INSTRUMENT);
    if (!hasFeature(d, CLAP_PLUGIN_FEATURE_INSTRUMENT) && !hasFeature(d, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT)) continue;
    out.push_back(info);
  }
  return out;
}
}
