#ifndef __MOD_GOOGLE_TRANSCRIBE_H__
#define __MOD_GOOGLE_TRANSCRIBE_H__

#include <switch.h>
#include <speex/speex_resampler.h>

#include <unistd.h>

#ifdef __cplusplus
#include <atomic>
#endif

#define MAX_SESSION_ID (256)
#define MAX_BUG_LEN (64)
#define MY_BUG_NAME "google_transcribe"
#define TRANSCRIBE_EVENT_RESULTS "google_transcribe::transcription"
#define TRANSCRIBE_EVENT_END_OF_UTTERANCE "google_transcribe::end_of_utterance"
#define TRANSCRIBE_EVENT_START_OF_TRANSCRIPT "google_transcribe::start_of_transcript"
#define TRANSCRIBE_EVENT_END_OF_TRANSCRIPT "google_transcribe::end_of_transcript"
#define TRANSCRIBE_EVENT_NO_AUDIO_DETECTED "google_transcribe::no_audio_detected"
#define TRANSCRIBE_EVENT_MAX_DURATION_EXCEEDED "google_transcribe::max_duration_exceeded"
#define TRANSCRIBE_EVENT_PLAY_INTERRUPT "google_transcribe::play_interrupt"
#define TRANSCRIBE_EVENT_VAD_DETECTED "google_transcribe::vad_detected"
#define TRANSCRIBE_EVENT_ERROR      "jambonz_transcribe::error"


// simply write a wave file
//#define DEBUG_TRANSCRIBE 0


#ifdef DEBUG_TRANSCRIBE

/* per-channel data */
struct cap_cb {
	switch_buffer_t *buffer;
	switch_mutex_t *mutex;
	char *base;
    SpeexResamplerState *resampler;
    FILE* fp;
};
#else
/* per-channel data */
typedef void (*responseHandler_t)(switch_core_session_t* session, const char* json, const char* bugname);

struct cap_cb {
	switch_mutex_t *mutex;
	char bugname[MAX_BUG_LEN+1];
	char sessionId[MAX_SESSION_ID+1];
	char *base;
	uint32_t channels;   /* capture channel count (2 with SMBF_STEREO) */
  SpeexResamplerState *resampler;
	/* streamer is read by the media (frame) thread at the gate BEFORE the
	 * trylock, while cleanup nulls it under cb->mutex after switch_thread_join.
	 * That gate read is unsynchronized against the cleanup store, so it is a
	 * lock-free atomic pointer (avoids the cleanup/thread_join deadlock a mutex
	 * would cause). Touched ONLY from google_glue.cpp (C++); std::atomic<void*>
	 * is lock-free and same-sized as void*, so the C ABI view and the
	 * zeroed-alloc layout are unchanged. */
#ifdef __cplusplus
	std::atomic<void*> streamer;
#else
	void* streamer;
#endif
	responseHandler_t responseHandler;
	switch_thread_t* thread;
  int wants_single_utterance;
  /* got_end_of_utterance is written by the gRPC read thread and read by the
   * media (frame) thread with no shared lock. A mutex here would deadlock
   * (cleanup holds cb->mutex across switch_thread_join vs the read thread), so
   * it is a lock-free atomic. It is touched ONLY from google_glue.cpp (C++);
   * the .c view stays a plain int and std::atomic<int> is layout-compatible
   * with int, so struct size/layout is unchanged. */
#ifdef __cplusplus
  std::atomic<int> got_end_of_utterance;
#else
  int got_end_of_utterance;
#endif
	int play_file;
	switch_vad_t * vad;
	uint32_t samples_per_second;
};
#endif

#endif