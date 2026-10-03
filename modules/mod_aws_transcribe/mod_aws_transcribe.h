#ifndef __MOD_AWS_TRANSCRIBE_H__
#define __MOD_AWS_TRANSCRIBE_H__

#include <switch.h>
#include <speex/speex_resampler.h>

#include <unistd.h>

#ifdef __cplusplus
#include <atomic>
#endif

#define MY_BUG_NAME "aws_transcribe"
#define MAX_BUG_LEN (64)
#define MAX_SESSION_ID (256)
#define TRANSCRIBE_EVENT_RESULTS "aws_transcribe::transcription"
#define TRANSCRIBE_EVENT_END_OF_TRANSCRIPT "aws_transcribe::end_of_transcript"
#define TRANSCRIBE_EVENT_NO_AUDIO_DETECTED "aws_transcribe::no_audio_detected"
#define TRANSCRIBE_EVENT_MAX_DURATION_EXCEEDED "aws_transcribe::max_duration_exceeded"
#define TRANSCRIBE_EVENT_VAD_DETECTED "aws_transcribe::vad_detected"
#define TRANSCRIBE_EVENT_ERROR      "jambonz_transcribe::error"

#define MAX_LANG (12)
#define MAX_REGION (32)

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

/* per-channel data */
typedef void (*responseHandler_t)(switch_core_session_t* session, const char * json, const char* bugname);

struct cap_cb {
	switch_mutex_t *mutex;
	char bugname[MAX_BUG_LEN+1];
	char sessionId[MAX_SESSION_ID+1];
  char awsAccessKeyId[128];
  char awsSecretAccessKey[128];
	uint32_t channels;
  SpeexResamplerState *resampler;
	/* Published by the worker thread (aws_transcribe_thread) when the GStreamer is
	   created and cleared when it is destroyed; read by the media-bug frame thread
	   (aws_transcribe_frame) and the API stop/cleanup thread. The worker writes it
	   WITHOUT holding cb->mutex, so the mutex alone cannot order those accesses --
	   the pointer must be atomic. Touched only from C++ (the .c file never reads or
	   writes cb->streamer), so the atomic type is confined to the C++ translation
	   unit and the C ABI sees a plain void*. */
#ifdef __cplusplus
	std::atomic<void*> streamer;
#else
	void* streamer;
#endif
	responseHandler_t responseHandler;
	switch_thread_t* thread;
	int interim;

	char lang[MAX_LANG];
	char region[MAX_REGION];

	switch_vad_t * vad;
	uint32_t samples_per_second;
	/* Set by stop/cleanup BEFORE they load cb->streamer; re-checked by the
	   worker right AFTER it publishes cb->streamer. Closes the window where a
	   stop lands while the GStreamer constructor is still running: the stop
	   found no streamer to finish() and then blocked forever (VAD path) in
	   switch_thread_join on a worker nothing would ever wake. Same
	   C/C++ ABI pattern as streamer above (the .c file never touches it). */
#ifdef __cplusplus
	std::atomic<int> stop_requested;
#else
	int stop_requested;
#endif
	/* unload-gate bookkeeping: set when the session counted itself in
	   session_init; killcb releases the count exactly once. Plain int: only
	   touched under cb->mutex or before the worker thread exists. */
	int gate_counted;
};

/* the session-less final-utterance fire (#241): a call that ends
   mid-utterance still gets AWS's final response after the channel's
   close; deliver it with only Unique-ID + vendor + media-bugname, the
   headers the ESL consumer routes on. Defined in the .c, called from
   the glue (.cpp) — so the declaration needs C linkage on the C++ side. */
#ifdef __cplusplus
extern "C" {
#endif
void aws_fire_session_less(const char* sessionId, const char* json, const char* bugname);
#ifdef __cplusplus
}
#endif

#endif