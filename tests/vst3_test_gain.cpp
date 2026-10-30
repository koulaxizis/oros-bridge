// A tiny VST 3 plugin for the bridge's tests (MIT, orOS): out = in x gain,
// gain is parameter 0 (default 0.5) and the whole state.
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioprocessoralgo.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

static const FUID kUid(0x6F726F73, 0x42726964, 0x67655465, 0x73744731);   // "orosBridgeTestG1"

class TestGain : public SingleComponentEffect {
public:
  float gain = 0.5f;
  static FUnknown* create(void*) { return (IAudioProcessor*)new TestGain; }
  tresult PLUGIN_API initialize(FUnknown* ctx) override {
    tresult r = SingleComponentEffect::initialize(ctx);
    if (r != kResultOk) return r;
    addAudioInput(STR16("In"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Out"), SpeakerArr::kStereo);
    parameters.addParameter(STR16("Gain"), nullptr, 0, 0.5, ParameterInfo::kCanAutomate, 0);
    return kResultOk;
  }
  tresult PLUGIN_API setBusArrangements(SpeakerArrangement* in, int32 nIn, SpeakerArrangement* out, int32 nOut) override {
    if (nIn == 1 && nOut == 1 && in[0] == SpeakerArr::kStereo && out[0] == SpeakerArr::kStereo)
      return SingleComponentEffect::setBusArrangements(in, nIn, out, nOut);
    return kResultFalse;
  }
  tresult PLUGIN_API process(ProcessData& d) override {
    if (d.inputParameterChanges) {
      for (int32 i = 0; i < d.inputParameterChanges->getParameterCount(); i++) {
        auto* q = d.inputParameterChanges->getParameterData(i);
        ParamValue v;
        int32 off;
        if (q && q->getParameterId() == 0 && q->getPointCount() > 0 && q->getPoint(q->getPointCount() - 1, off, v) == kResultOk)
          gain = float(v);
      }
    }
    if (d.numInputs < 1 || d.numOutputs < 1) return kResultOk;
    for (int32 c = 0; c < 2; c++)
      for (int32 i = 0; i < d.numSamples; i++)
        d.outputs[0].channelBuffers32[c][i] = d.inputs[0].channelBuffers32[c][i] * gain;
    return kResultOk;
  }
  tresult PLUGIN_API setState(IBStream* s) override {
    float g;
    int32 n = 0;
    if (s->read(&g, 4, &n) != kResultOk || n != 4) return kResultFalse;
    gain = g;
    setParamNormalized(0, g);
    return kResultOk;
  }
  tresult PLUGIN_API getState(IBStream* s) override {
    int32 n = 0;
    return s->write(&gain, 4, &n);
  }
};

BEGIN_FACTORY_DEF("orOS", "https://useoros.online", "mailto:hello@useoros.online")
DEF_CLASS2(INLINE_UID_FROM_FUID(kUid), PClassInfo::kManyInstances, kVstAudioEffectClass, "orOS Test VST3 Gain",
           Vst::kDistributable, "Fx", "1.0.0", kVstVersionString, TestGain::create)
END_FACTORY
