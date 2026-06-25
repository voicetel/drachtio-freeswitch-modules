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
};

#endif