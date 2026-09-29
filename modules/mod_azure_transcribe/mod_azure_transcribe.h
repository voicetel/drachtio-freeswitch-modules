#ifndef __MOD_AZURE_TRANSCRIBE_H__
#define __MOD_AZURE_TRANSCRIBE_H__

#include <switch.h>
#include <speex/speex_resampler.h>

#include <unistd.h>

#define MY_BUG_NAME "azure_transcribe"
#define MAX_BUG_LEN (64)
#define TRANSCRIBE_EVENT_RESULTS "azure_transcribe::transcription"
#define TRANSCRIBE_EVENT_START_OF_UTTERANCE "azure_transcribe::start_of_utterance"
#define TRANSCRIBE_EVENT_END_OF_UTTERANCE "azure_transcribe::end_of_utterance"
#define TRANSCRIBE_EVENT_NO_SPEECH_DETECTED "azure_transcribe::no_speech_detected"
#define TRANSCRIBE_EVENT_VAD_DETECTED "azure_transcribe::vad_detected"
#define TRANSCRIBE_EVENT_ERROR      "jambonz_transcribe::error"

#define MAX_REGION (32)
#define MAX_SUBSCRIPTION_KEY_LEN (256)

/* Frame buffer size for switch_core_media_bug_read() on an SMBF_STEREO bug.
   Upstream FS hazard (src/switch_core_media_bug.c): the stereo path ends with
   memcpy(frame->data, bug->tmp, bytes * 2) where bytes is
   read_impl.decoded_bytes_per_packet, but the buflen guard at the top of the
   function only compares buflen against the MONO count. A caller passing
   SWITCH_RECOMMENDED_BUFFER_SIZE is therefore under-sized by exactly 2x for
   stereo. 2x is the exact worst case, not a heuristic: FS rejects any codec
   implementation whose decoded_bytes_per_packet exceeds
   SWITCH_RECOMMENDED_BUFFER_SIZE at registration
   (switch_core_codec_add_implementation), so the write can never exceed this.
   Shared by all five maintained modules (see their headers). */
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
typedef void (*responseHandler_t)(switch_core_session_t* session, const char* event, const char * json, const char* bugname, int finished);

struct cap_cb {
	switch_mutex_t *mutex;
	 char bugname[MAX_BUG_LEN+1];
  char subscriptionKey[MAX_SUBSCRIPTION_KEY_LEN];
	uint32_t channels;
  SpeexResamplerState *resampler;
	void* streamer;
	responseHandler_t responseHandler;

	char region[MAX_REGION];

	switch_vad_t * vad;
};

#endif