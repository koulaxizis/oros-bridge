// Hosting VST 3 plugins (Steinberg VST 3 SDK 3.8.1, MIT).
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "editor.h"
#include "mainloop.h"
#include "plugin.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace ob {
namespace {

HostApplication& hostApp() {
  static HostApplication app;
  static bool set = false;
  if (!set) {
    PluginContextFactory::instance().setPluginContext(&app);
    set = true;
  }
  return app;
}

bool isInstrument(const VST3::Hosting::ClassInfo& ci) {
  for (auto& s : ci.subCategories())
    if (s == "Instrument" || s.rfind("Instrument|", 0) == 0) return true;
  return ci.subCategoriesString().find("Instrument") != std::string::npos;
}

// Knob turns in the plugin's window reach the audio side through the host.
class Handler : public U::ImplementsNonDestroyable<U::Directly<IComponentHandler>> {
public:
  std::mutex mu;
  std::vector<std::pair<ParamID, ParamValue>> pending;
  std::atomic<bool> dirty{false};
  std::atomic<bool> latencyChanged{false};
  tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
  tresult PLUGIN_API performEdit(ParamID id, ParamValue v) override {
    std::lock_guard<std::mutex> g(mu);
    pending.emplace_back(id, v);
    dirty = true;
    return kResultOk;
  }
  tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
  tresult PLUGIN_API restartComponent(int32 flags) override {
    if (flags & kLatencyChanged) latencyChanged = true;
    dirty = true;
    return kResultOk;
  }
};

class Frame : public U::ImplementsNonDestroyable<U::Directly<IPlugFrame>> {
public:
  EditorWindow* win = nullptr;
  IPlugView* view = nullptr;
  tresult PLUGIN_API resizeView(IPlugView* v, ViewRect* r) override {
    if (!win || !r) return kResultFalse;
    win->resize(r->getWidth(), r->getHeight());
    if (v) v->onSize(r);
    return kResultTrue;
  }
};

class Vst3Plugin : public Plugin {
public:
  VST3::Hosting::Module::Ptr module;
  IPtr<PlugProvider> provider;
  IPtr<IComponent> component;
  IPtr<IEditController> controller;
  FUnknownPtr<IAudioProcessor> processor;
  Handler handler;
  Frame frame;
  HostProcessData data;
  EventList events{512};
  ParameterChanges changes{1024};
  ProcessContext context{};
  uint32_t ins = 0, outs = 0, maxFrames = 0;
  bool active = false, processing = false, realtime = false;
  std::vector<std::vector<float>> scratch;
  IPtr<IPlugView> view;
  std::unique_ptr<EditorWindow> win;
  std::function<void()> onEditorClosed;

  ~Vst3Plugin() override {
    closeEditor();
    stop();
    if (controller) controller->setComponentHandler(nullptr);
    data.unprepare();
    processor = nullptr;
    controller = nullptr;
    component = nullptr;
    provider = nullptr;
    module = nullptr;
  }

  void stop() {
    if (processing && processor) processor->setProcessing(false);
    processing = false;
    if (active && component) component->setActive(false);
    active = false;
  }

  uint32_t busChannels(BusDirection dir) {
    if (component->getBusCount(kAudio, dir) < 1) return 0;
    BusInfo bi{};
    if (component->getBusInfo(kAudio, dir, 0, bi) != kResultOk) return 0;
    return std::min<uint32_t>(uint32_t(bi.channelCount), kMaxChannels);
  }

  bool load(const PluginInfo& info, std::string& err) {
    hostApp();
    module = VST3::Hosting::Module::create(info.path, err);
    if (!module) return false;
    auto factory = module->getFactory();
    factory.setHostContext(&hostApp());
    std::string uid = info.id.substr(5);   // after "vst3:"
    for (auto& ci : factory.classInfos()) {
      if (ci.category() != kVstAudioEffectClass || ci.ID().toString() != uid) continue;
      provider = owned(new PlugProvider(factory, ci, true));
      if (!provider->initialize()) { provider = nullptr; err = "the plugin did not start"; return false; }
      break;
    }
    if (!provider) { err = "the plugin is no longer in its file"; return false; }
    component = provider->getComponentPtr();
    controller = provider->getControllerPtr();
    processor = FUnknownPtr<IAudioProcessor>(component);
    if (!component || !processor) { err = "not an audio plugin"; return false; }
    if (controller) controller->setComponentHandler(&handler);
    // stereo where the plugin can do it
    int32 nIn = component->getBusCount(kAudio, kInput), nOut = component->getBusCount(kAudio, kOutput);
    std::vector<SpeakerArrangement> ai(nIn, SpeakerArr::kStereo), ao(nOut, SpeakerArr::kStereo);
    for (int32 i = 0; i < nIn; i++) processor->getBusArrangement(kInput, i, ai[i]);
    for (int32 i = 0; i < nOut; i++) processor->getBusArrangement(kOutput, i, ao[i]);
    if (nIn > 0) ai[0] = SpeakerArr::kStereo;
    if (nOut > 0) ao[0] = SpeakerArr::kStereo;
    processor->setBusArrangements(ai.data(), nIn, ao.data(), nOut);   // the plugin may refuse: keep what it has
    if (nIn > 0) component->activateBus(kAudio, kInput, 0, true);
    if (nOut > 0) component->activateBus(kAudio, kOutput, 0, true);
    if (component->getBusCount(kEvent, kInput) > 0) component->activateBus(kEvent, kInput, 0, true);
    ins = busChannels(kInput);
    outs = busChannels(kOutput);
    if (!outs) { err = "the plugin has no audio output"; return false; }
    return true;
  }

  bool activate(double sr, uint32_t frames, bool rt, std::string& err) override {
    stop();
    realtime = rt;
    ProcessSetup setup{rt ? kRealtime : kOffline, kSample32, int32(frames), sr};
    if (processor->setupProcessing(setup) != kResultOk) { err = "the plugin refused the sample rate or block size"; return false; }
    data.prepare(*component, 0, kSample32);
    data.inputEvents = &events;
    data.inputParameterChanges = &changes;
    data.processContext = &context;
    context = {};
    context.sampleRate = sr;
    context.tempo = 120;
    context.state = ProcessContext::kTempoValid;
    maxFrames = frames;
    size_t need = 0;
    for (int32 b = 0; b < data.numInputs; b++) need += size_t(data.inputs[b].numChannels);
    for (int32 b = 0; b < data.numOutputs; b++) need += size_t(data.outputs[b].numChannels);
    scratch.assign(need, std::vector<float>(frames, 0.f));
    if (component->setActive(true) != kResultOk) { err = "the plugin could not be switched on"; return false; }
    active = true;
    processor->setProcessing(true);   // many plugins return kNotImplemented here
    processing = true;
    return true;
  }
  uint32_t inputs() const override { return ins; }
  uint32_t outputs() const override { return outs; }
  uint32_t latency() override { return processor ? processor->getLatencySamples() : 0; }
  uint32_t tail() override {
    if (!processor) return 0;
    uint32 t = processor->getTailSamples();
    if (t == kInfiniteTail) return kMaxTail;
    return std::min<uint32_t>(t, kMaxTail);
  }

  bool process(const float* const* in, float* const* out, uint32_t frames, const std::vector<MidiEvent>& ev) override {
    if (!active || frames > maxFrames) return false;
    size_t s = 0;
    for (int32 b = 0; b < data.numInputs; b++)
      for (int32 c = 0; c < data.inputs[b].numChannels; c++) {
        float* buf;
        if (b == 0 && uint32_t(c) < ins) buf = const_cast<float*>(in[c]);
        else { buf = scratch[s++].data(); std::fill(buf, buf + frames, 0.f); }
        data.setChannelBuffer(kInput, b, c, buf);
      }
    for (int32 b = 0; b < data.numOutputs; b++)
      for (int32 c = 0; c < data.outputs[b].numChannels; c++)
        data.setChannelBuffer(kOutput, b, c, b == 0 && uint32_t(c) < outs ? out[c] : scratch[s++].data());
    for (int32 b = 0; b < data.numInputs; b++) data.inputs[b].silenceFlags = 0;
    for (int32 b = 0; b < data.numOutputs; b++) data.outputs[b].silenceFlags = 0;
    events.clear();
    for (auto& m : ev) {
      Event e{};
      e.busIndex = 0;
      e.sampleOffset = int32(std::min(m.offset, frames - 1));
      uint8_t kind = m.status & 0xF0;
      if (kind == 0x90 && m.d2 > 0) {
        e.type = Event::kNoteOnEvent;
        e.noteOn.channel = m.status & 15;
        e.noteOn.pitch = m.d1 & 127;
        e.noteOn.velocity = m.d2 / 127.f;
        e.noteOn.noteId = -1;
      } else if (kind == 0x80 || kind == 0x90) {
        e.type = Event::kNoteOffEvent;
        e.noteOff.channel = m.status & 15;
        e.noteOff.pitch = m.d1 & 127;
        e.noteOff.velocity = kind == 0x80 ? m.d2 / 127.f : 0.f;
        e.noteOff.noteId = -1;
      } else {
        continue;   // controllers need the plugin's MIDI mapping: later
      }
      events.addEvent(e);
    }
    changes.clearQueue();
    // the edits from the plugin's window; on a sound card's thread only if the window is not adding one now
    std::unique_lock<std::mutex> g(handler.mu, std::defer_lock);
    if (realtime ? g.try_lock() : (g.lock(), true)) {
      for (auto& pv : handler.pending) {
        int32 idx = 0, pidx = 0;
        if (auto* q = changes.addParameterData(pv.first, idx)) q->addPoint(0, pv.second, pidx);
      }
      handler.pending.clear();
    }
    data.numSamples = int32(frames);
    bool ok = processor->process(data) == kResultOk;
    context.projectTimeSamples += frames;
    context.continousTimeSamples += frames;
    return ok;
  }

  void reset() override {
    // switching off and on is how VST 3 forgets its sound
    if (!active) return;
    if (processing) processor->setProcessing(false);
    component->setActive(false);
    component->setActive(true);
    processor->setProcessing(true);
    context.projectTimeSamples = 0;
    context.continousTimeSamples = 0;
  }

  static void putBlock(std::vector<uint8_t>& out, MemoryStream& s) {
    uint32_t n = uint32_t(s.getSize());
    for (int i = 0; i < 4; i++) out.push_back(uint8_t(n >> (i * 8)));
    out.insert(out.end(), s.getData(), s.getData() + n);
  }
  // state: "OBV3", then the component's and the controller's, each with its length
  bool getState(std::vector<uint8_t>& out) override {
    out = {'O', 'B', 'V', '3'};
    MemoryStream cs, es;
    if (component->getState(&cs) != kResultOk) cs.setSize(0);
    if (controller && controller->getState(&es) != kResultOk) es.setSize(0);
    putBlock(out, cs);
    putBlock(out, es);
    handler.dirty = false;
    return true;
  }
  bool setState(const std::vector<uint8_t>& in) override {
    if (in.size() < 12 || std::memcmp(in.data(), "OBV3", 4) != 0) return false;
    size_t pos = 4;
    auto block = [&](std::vector<uint8_t>& b) {
      if (pos + 4 > in.size()) return false;
      uint32_t n = in[pos] | in[pos + 1] << 8 | in[pos + 2] << 16 | uint32_t(in[pos + 3]) << 24;
      pos += 4;
      if (n > in.size() - pos) return false;
      b.assign(in.begin() + pos, in.begin() + pos + n);
      pos += n;
      return true;
    };
    std::vector<uint8_t> c, e;
    if (!block(c) || !block(e)) return false;
    if (!c.empty()) {
      MemoryStream s1(c.data(), TSize(c.size()));
      if (component->setState(&s1) != kResultOk) return false;
      if (controller) {
        MemoryStream s2(c.data(), TSize(c.size()));
        controller->setComponentState(&s2);
      }
    }
    if (!e.empty() && controller) {
      MemoryStream s3(e.data(), TSize(e.size()));
      controller->setState(&s3);
    }
    handler.dirty = false;
    return true;
  }

  bool hasEditor() override {
#ifdef _WIN32
    if (!controller) return false;
    IPtr<IPlugView> v = owned(controller->createView(ViewType::kEditor));
    return v && v->isPlatformTypeSupported(kPlatformTypeHWND) == kResultTrue;
#else
    return false;
#endif
  }
  bool openEditor(const std::string& title, std::function<void()> onClosed, std::string& err) override {
    if (view) { if (win) win->show(); return true; }
#ifdef _WIN32
    if (!controller) { err = "this plugin has no window"; return false; }
    view = owned(controller->createView(ViewType::kEditor));
    if (!view || view->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue) { view = nullptr; err = "this plugin has no window here"; return false; }
    ViewRect r{};
    view->getSize(&r);
    onEditorClosed = std::move(onClosed);
    win = EditorWindow::create(title, r.getWidth(), r.getHeight(), view->canResize() == kResultTrue,
                               [this] { editorClosedByPerson(); },
                               [this](int w, int h) {
                                 if (!view) return;
                                 ViewRect nr{0, 0, w, h};
                                 if (view->checkSizeConstraint(&nr) == kResultTrue) view->onSize(&nr);
                               });
    if (!win) { view = nullptr; err = "no window"; return false; }
    frame.win = win.get();
    frame.view = view;
    view->setFrame(&frame);
    if (view->attached(win->nativeHandle(), kPlatformTypeHWND) != kResultTrue) {
      view->setFrame(nullptr);
      view = nullptr;
      win.reset();
      err = "the plugin window could not be placed";
      return false;
    }
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
    if (view) {
      view->removed();
      view->setFrame(nullptr);
      view = nullptr;
    }
    frame.win = nullptr;
    frame.view = nullptr;
    win.reset();
    onEditorClosed = nullptr;
  }
  bool dirty() override { return handler.dirty; }
};
}

std::unique_ptr<Plugin> openVst3(const PluginInfo& info, std::string& err) {
  auto pl = std::make_unique<Vst3Plugin>();
  if (!pl->load(info, err)) return nullptr;
  return pl;
}

std::vector<PluginInfo> describeVst3(const std::string& path, std::string& err) {
  std::vector<PluginInfo> out;
  hostApp();
  auto module = VST3::Hosting::Module::create(path, err);
  if (!module) return out;
  for (auto& ci : module->getFactory().classInfos()) {
    if (ci.category() != kVstAudioEffectClass) continue;
    PluginInfo info;
    info.id = "vst3:" + ci.ID().toString();
    info.format = "vst3";
    info.name = ci.name();
    info.vendor = ci.vendor();
    info.version = ci.version();
    info.path = path;
    info.instrument = isInstrument(ci);
    out.push_back(info);
  }
  return out;
}
}
