/* orOS Bridge audio modules: a sound system in a separate library that the
 * bridge loads only when it is present (oros-bridge-asio.dll beside the exe,
 * or --audio-module PATH). The library can carry its own licence (the ASIO
 * module is GPLv3, asio/); this header is the whole boundary and is MIT, so
 * either side can include it. Plain C, no allocation across the boundary.
 *
 * The library exports one function, OROS_AUDIO_MODULE_ENTRY, which returns
 * its table for the ABI version the host asks for, or NULL. Every string is
 * UTF-8. Calls other than the stream callback come from one host thread at a
 * time. */
#ifndef OROS_AUDIO_MODULE_H
#define OROS_AUDIO_MODULE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OROS_AUDIO_MODULE_ABI 1
#define OROS_AUDIO_MODULE_ENTRY "oros_audio_module_v1"

/* One period: `in` holds inCh interleaved floats per frame, `out` outCh; the
 * callback fills `out` completely. Called on the sound card's own thread. */
typedef void (*oros_audio_cb)(void* user, const float* in, uint32_t inCh, float* out, uint32_t outCh, uint32_t frames);

typedef struct oros_audio_stream_info {
  uint32_t sampleRate, inChannels, outChannels, period;
  uint32_t latency; /* frames from an output sample to its return on an input */
} oros_audio_stream_info;

typedef struct oros_audio_module {
  uint32_t abi;      /* OROS_AUDIO_MODULE_ABI */
  const char* id;    /* "asio" */
  const char* label; /* shown to the person */
  /* The cards, as JSON {"inputs":[{name,channels,default}],"outputs":[...]}.
   * Returns the length the JSON needs including its NUL; when that is more
   * than cap, nothing useful was written and the host asks again with more.
   * Returns 0 on failure with a message in err. */
  uint32_t (*devices)(char* json, uint32_t cap, char* err, uint32_t errCap);
  /* Open and start a duplex stream; "" means the default card. NULL on
   * failure with a message in err. */
  void* (*open)(const char* input, const char* output, uint32_t sampleRate, uint32_t period, oros_audio_cb cb,
                void* user, oros_audio_stream_info* info, char* err, uint32_t errCap);
  /* Stop the callback, then free the stream. */
  void (*close)(void* stream);
} oros_audio_module;

typedef const oros_audio_module* (*oros_audio_module_entry_fn)(uint32_t hostAbi);

#ifdef __cplusplus
}
#endif
#endif
