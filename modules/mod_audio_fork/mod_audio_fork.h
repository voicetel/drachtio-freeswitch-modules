#ifndef __MOD_FORK_H__
#define __MOD_FORK_H__

#include <switch.h>
#include <libwebsockets.h>
#include <speex/speex_resampler.h>

#include <unistd.h>

#ifdef __cplusplus
#include <atomic>
#endif

#define MY_BUG_NAME "audio_fork"
#define MAX_BUG_LEN (64)
#define MAX_SESSION_ID (256)
#define MAX_WS_URL_LEN (512)
#define MAX_PATH_LEN (4096)

#define EVENT_TRANSCRIPTION   "mod_audio_fork::transcription"
#define EVENT_TRANSFER        "mod_audio_fork::transfer"
#define EVENT_PLAY_AUDIO      "mod_audio_fork::play_audio"
#define EVENT_KILL_AUDIO      "mod_audio_fork::kill_audio"
#define EVENT_DISCONNECT      "mod_audio_fork::disconnect"
#define EVENT_ERROR           "mod_audio_fork::error"
#define EVENT_CONNECT_SUCCESS "mod_audio_fork::connect"
#define EVENT_CONNECT_FAIL    "mod_audio_fork::connect_failed"
#define EVENT_BUFFER_OVERRUN  "mod_audio_fork::buffer_overrun"
#define EVENT_JSON            "mod_audio_fork::json"

#define MAX_METADATA_LEN (8192)

struct playout {
  char *file;
  struct playout* next;
};

typedef void (*responseHandler_t)(switch_core_session_t* session, const char* eventName, char* json);

struct private_data {
	switch_mutex_t *mutex;
	char sessionId[MAX_SESSION_ID];
  char bugname[MAX_BUG_LEN+1];
  SpeexResamplerState *resampler;
  responseHandler_t responseHandler;
  void *pAudioPipe;
  int ws_state;
  char host[MAX_WS_URL_LEN];
  unsigned int port;
  char path[MAX_PATH_LEN];
  int sampling;
  struct playout* playout;
  int  channels;
  unsigned int id;
  int buffer_overrun_notified:1;
  /* audio_paused and graceful_shutdown are read on the media-bug (fork_frame)
     thread and written on the API command thread, with no common lock at the
     read site (the read happens before fork_frame's trylock). Make them atomic
     so the cross-thread read/write does not race. They are touched ONLY from
     C++ (lws_glue.cpp); mod_audio_fork.c never references them, so std::atomic<int>
     (layout-compatible with int) is ABI-safe for the C side of this shared struct.
     These cannot be :1 bitfields because std::atomic requires a full object. */
#ifdef __cplusplus
  std::atomic<int> audio_paused;
  std::atomic<int> graceful_shutdown;
#else
  int audio_paused;
  int graceful_shutdown;
#endif
  char initialMetadata[8192];
};

typedef struct private_data private_t;

#endif
