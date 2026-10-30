/* Two tiny CLAP plugins for the bridge's tests (MIT, orOS):
 *   oros.test.gain  effect: out = in delayed by 32 frames (latency 32) x gain (state, default 0.5)
 *   oros.test.sine  instrument: a sine per held note, amplitude = velocity / 4 */
#include <clap/clap.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DELAY 32

typedef struct {
  clap_plugin_t plugin;
  const clap_host_t *host;
  int sine;
  float gain;
  float hist[2][DELAY];
  int pos;
  double sr;
  double phase[128];
  double amp[128];
} test_plugin;

static const char *gain_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL};
static const char *sine_features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, NULL};
static const clap_plugin_descriptor_t gain_desc = {CLAP_VERSION_INIT, "oros.test.gain", "orOS Test Gain", "orOS", "", "", "", "1.0.0", "", gain_features};
static const clap_plugin_descriptor_t sine_desc = {CLAP_VERSION_INIT, "oros.test.sine", "orOS Test Sine", "orOS", "", "", "", "1.0.0", "", sine_features};

static bool p_init(const clap_plugin_t *p) { (void)p; return true; }
static void p_destroy(const clap_plugin_t *p) { free(p->plugin_data); }
static bool p_activate(const clap_plugin_t *p, double sr, uint32_t a, uint32_t b) {
  (void)a; (void)b;
  test_plugin *t = p->plugin_data;
  t->sr = sr;
  return true;
}
static void p_deactivate(const clap_plugin_t *p) { (void)p; }
static bool p_start(const clap_plugin_t *p) { (void)p; return true; }
static void p_stop(const clap_plugin_t *p) { (void)p; }
static void p_reset(const clap_plugin_t *p) {
  test_plugin *t = p->plugin_data;
  memset(t->hist, 0, sizeof t->hist);
  t->pos = 0;
  memset(t->amp, 0, sizeof t->amp);
  memset(t->phase, 0, sizeof t->phase);
}

static void handle(test_plugin *t, const clap_event_header_t *h) {
  if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
  if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF) {
    const clap_event_note_t *n = (const clap_event_note_t *)h;
    if (n->key < 0 || n->key > 127) return;
    t->amp[n->key] = h->type == CLAP_EVENT_NOTE_ON ? n->velocity / 4.0 : 0.0;
  }
}

static clap_process_status p_process(const clap_plugin_t *p, const clap_process_t *pr) {
  test_plugin *t = p->plugin_data;
  uint32_t nev = pr->in_events->size(pr->in_events), ei = 0;
  for (uint32_t i = 0; i < pr->frames_count; i++) {
    while (ei < nev) {
      const clap_event_header_t *h = pr->in_events->get(pr->in_events, ei);
      if (h->time > i) break;
      handle(t, h);
      ei++;
    }
    if (t->sine) {
      double s = 0;
      for (int k = 0; k < 128; k++) {
        if (t->amp[k] == 0) continue;
        double f = 440.0 * pow(2.0, (k - 69) / 12.0);
        s += t->amp[k] * sin(t->phase[k]);
        t->phase[k] += 2 * M_PI * f / t->sr;
      }
      pr->audio_outputs[0].data32[0][i] = (float)s;
      pr->audio_outputs[0].data32[1][i] = (float)s;
    } else {
      for (int c = 0; c < 2; c++) {
        float in = pr->audio_inputs[0].data32[c][i];
        pr->audio_outputs[0].data32[c][i] = t->hist[c][t->pos] * t->gain;
        t->hist[c][t->pos] = in;
      }
      t->pos = (t->pos + 1) % DELAY;
    }
  }
  while (ei < nev) handle(t, pr->in_events->get(pr->in_events, ei++));
  return CLAP_PROCESS_CONTINUE;
}

static uint32_t ports_count(const clap_plugin_t *p, bool is_input) {
  test_plugin *t = p->plugin_data;
  return is_input && t->sine ? 0 : 1;
}
static bool ports_get(const clap_plugin_t *p, uint32_t i, bool is_input, clap_audio_port_info_t *info) {
  (void)p; (void)is_input;
  if (i) return false;
  memset(info, 0, sizeof *info);
  info->id = 0;
  strcpy(info->name, "main");
  info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}
static const clap_plugin_audio_ports_t ports = {ports_count, ports_get};

static uint32_t notes_count(const clap_plugin_t *p, bool is_input) {
  test_plugin *t = p->plugin_data;
  return t->sine && is_input ? 1 : 0;
}
static bool notes_get(const clap_plugin_t *p, uint32_t i, bool is_input, clap_note_port_info_t *info) {
  (void)p; (void)is_input;
  if (i) return false;
  memset(info, 0, sizeof *info);
  info->supported_dialects = CLAP_NOTE_DIALECT_CLAP;
  info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
  strcpy(info->name, "notes");
  return true;
}
static const clap_plugin_note_ports_t notes = {notes_count, notes_get};

static bool state_save(const clap_plugin_t *p, const clap_ostream_t *s) {
  test_plugin *t = p->plugin_data;
  return s->write(s, &t->gain, 4) == 4;
}
static bool state_load(const clap_plugin_t *p, const clap_istream_t *s) {
  test_plugin *t = p->plugin_data;
  float g;
  if (s->read(s, &g, 4) != 4) return false;
  t->gain = g;
  return true;
}
static const clap_plugin_state_t state = {state_save, state_load};

static uint32_t latency_get(const clap_plugin_t *p) {
  test_plugin *t = p->plugin_data;
  return t->sine ? 0 : DELAY;
}
static const clap_plugin_latency_t latency = {latency_get};

static const void *p_ext(const clap_plugin_t *p, const char *id) {
  test_plugin *t = p->plugin_data;
  if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ports;
  if (!strcmp(id, CLAP_EXT_STATE)) return &state;
  if (!strcmp(id, CLAP_EXT_LATENCY)) return &latency;
  if (!strcmp(id, CLAP_EXT_NOTE_PORTS) && t->sine) return &notes;
  return NULL;
}
static void p_main(const clap_plugin_t *p) { (void)p; }

static const clap_plugin_t *create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id) {
  (void)f;
  int sine;
  if (!strcmp(id, gain_desc.id)) sine = 0;
  else if (!strcmp(id, sine_desc.id)) sine = 1;
  else return NULL;
  test_plugin *t = calloc(1, sizeof *t);
  t->host = host;
  t->sine = sine;
  t->gain = 0.5f;
  t->sr = 48000;
  t->plugin.desc = sine ? &sine_desc : &gain_desc;
  t->plugin.plugin_data = t;
  t->plugin.init = p_init;
  t->plugin.destroy = p_destroy;
  t->plugin.activate = p_activate;
  t->plugin.deactivate = p_deactivate;
  t->plugin.start_processing = p_start;
  t->plugin.stop_processing = p_stop;
  t->plugin.reset = p_reset;
  t->plugin.process = p_process;
  t->plugin.get_extension = p_ext;
  t->plugin.on_main_thread = p_main;
  return &t->plugin;
}
static uint32_t count(const clap_plugin_factory_t *f) { (void)f; return 2; }
static const clap_plugin_descriptor_t *desc(const clap_plugin_factory_t *f, uint32_t i) {
  (void)f;
  return i == 0 ? &gain_desc : i == 1 ? &sine_desc : NULL;
}
static const clap_plugin_factory_t factory = {count, desc, create};
static bool e_init(const char *path) { (void)path; return true; }
static void e_deinit(void) {}
static const void *e_factory(const char *id) { return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &factory : NULL; }

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, e_init, e_deinit, e_factory};
