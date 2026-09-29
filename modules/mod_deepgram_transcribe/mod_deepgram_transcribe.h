#ifndef __MOD_DEEPGRAM_TRANSCRIBE_H__
#define __MOD_DEEPGRAM_TRANSCRIBE_H__

#include <switch.h>
#include <speex/speex_resampler.h>

#include <unistd.h>

#define MY_BUG_NAME "deepgram_transcribe"
#define TRANSCRIBE_EVENT_RESULTS "deepgram_transcribe::transcription"
/* NB: no_audio_detected / vad_detected events are defined by sibling modules
   but this module never fires them; the defines were removed so nobody wires
   a consumer to an event that cannot happen */
#define TRANSCRIBE_EVENT_CONNECT_SUCCESS "deepgram_transcribe::connect"
#define TRANSCRIBE_EVENT_CONNECT_FAIL    "deepgram_transcribe::connect_failed"
#define TRANSCRIBE_EVENT_BUFFER_OVERRUN  "deepgram_transcribe::buffer_overrun"
#define TRANSCRIBE_EVENT_DISCONNECT      "deepgram_transcribe::disconnect"

#define MAX_SESSION_ID (256)
#define MAX_WS_URL_LEN (512)
#define MAX_PATH_LEN (4096)
#define MAX_BUG_LEN (64)

/* media_bug_read's SMBF_STEREO path writes 2x the buflen it guards against;
   this is the exact worst-case bound (full rationale in mod_azure_transcribe.h) */
#define MEDIA_BUG_FRAME_BUF_SIZE (2 * SWITCH_RECOMMENDED_BUFFER_SIZE)

#if (defined(__cplusplus) && __cplusplus >= 201103L) || \
    (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L)
/* drift guard: the factor must stay >= the SMBF_STEREO write of 2x
   decoded_bytes_per_packet. Fails the build if "tidied" back to 8192. */
#if defined(__cplusplus)
static_assert(MEDIA_BUG_FRAME_BUF_SIZE >= 2 * SWITCH_RECOMMENDED_BUFFER_SIZE,
              "media_bug_read SMBF_STEREO writes 2x decoded_bytes_per_packet");
#else
_Static_assert(MEDIA_BUG_FRAME_BUF_SIZE >= 2 * SWITCH_RECOMMENDED_BUFFER_SIZE,
               "media_bug_read SMBF_STEREO writes 2x decoded_bytes_per_packet");
#endif
#endif

typedef void (*responseHandler_t)(switch_core_session_t* session, const char* eventName, const char* json, const char* bugname, int finished);

struct private_data {
	switch_mutex_t *mutex;
	char sessionId[MAX_SESSION_ID];
  SpeexResamplerState *resampler;
  responseHandler_t responseHandler;
  void *pAudioPipe;
  char host[MAX_WS_URL_LEN];
  unsigned int port;
  char path[MAX_PATH_LEN];
  char bugname[MAX_BUG_LEN+1];
  int sampling;
  int  channels;
  unsigned int id;
  int buffer_overrun_notified:1;
};

typedef struct private_data private_t;

#endif