/* 
 *
 * mod_google_transcribe.c -- Freeswitch module for real-time transcription using google's gRPC interface
 *
 */
#include "mod_google_transcribe.h"
#include "google_glue.h"
#include <stdlib.h>
#include <switch.h>

static const uint32_t DEFAULT_SAMPLE_RATE = 8000;

/* Prototypes */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_transcribe_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_transcribe_runtime);
SWITCH_MODULE_LOAD_FUNCTION(mod_transcribe_load);

SWITCH_MODULE_DEFINITION(mod_google_transcribe, mod_transcribe_load, mod_transcribe_shutdown, NULL);

static switch_status_t do_stop(switch_core_session_t *session, char* bugname);

/* which event subclasses THIS module reserved at load (vs found already
   reserved by a sibling -- the error subclass name is deliberately shared).
   Only owned reservations are freed at unload. */
#define N_RESERVED_SUBCLASSES 9
static int g_reserved_subclasses[N_RESERVED_SUBCLASSES];


static void responseHandler(switch_core_session_t* session, const char * json, const char* bugname) {
	switch_event_t *event = NULL;
	switch_channel_t *channel = switch_core_session_get_channel(session);

	if (0 == strcmp("vad_detected", json)) {
		if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_VAD_DETECTED) != SWITCH_STATUS_SUCCESS || !event) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "failed to create event subclass %s\n", TRANSCRIBE_EVENT_VAD_DETECTED);
			return;
		}
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "google");
	}
	else if (0 == strcmp("end_of_utterance", json)) {
		if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_END_OF_UTTERANCE) != SWITCH_STATUS_SUCCESS || !event) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "failed to create event subclass %s\n", TRANSCRIBE_EVENT_END_OF_UTTERANCE);
			return;
		}
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "google");
	}
	else if (0 == strcmp("end_of_transcript", json)) {
		if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_END_OF_TRANSCRIPT) != SWITCH_STATUS_SUCCESS || !event) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "failed to create event subclass %s\n", TRANSCRIBE_EVENT_END_OF_TRANSCRIPT);
			return;
		}
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "google");
	}
	else if (0 == strcmp("start_of_transcript", json)) {
		if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_START_OF_TRANSCRIPT) != SWITCH_STATUS_SUCCESS || !event) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "failed to create event subclass %s\n", TRANSCRIBE_EVENT_START_OF_TRANSCRIPT);
			return;
		}
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "google");
	}
	else if (0 == strcmp("max_duration_exceeded", json)) {
		if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_MAX_DURATION_EXCEEDED) != SWITCH_STATUS_SUCCESS || !event) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "failed to create event subclass %s\n", TRANSCRIBE_EVENT_MAX_DURATION_EXCEEDED);
			return;
		}
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "google");
	}
	else if (0 == strcmp("no_audio", json)) {
		if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_NO_AUDIO_DETECTED) != SWITCH_STATUS_SUCCESS || !event) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "failed to create event subclass %s\n", TRANSCRIBE_EVENT_NO_AUDIO_DETECTED);
			return;
		}
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "google");
	}
	else if (0 == strcmp("play_interrupt", json)){
		switch_event_t *qevent;
		switch_status_t status;
		if (switch_event_create(&qevent, SWITCH_EVENT_DETECTED_SPEECH) == SWITCH_STATUS_SUCCESS) {
			if ((status = switch_core_session_queue_event(session, &qevent)) != SWITCH_STATUS_SUCCESS){
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "unable to queue play inturrupt event  %d \n", status);
			}
		}else{
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "unable to create play inturrupt event \n");
		}
		if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_PLAY_INTERRUPT) != SWITCH_STATUS_SUCCESS || !event) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "failed to create event subclass %s\n", TRANSCRIBE_EVENT_PLAY_INTERRUPT);
			return;
		}
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "google");
	}
	else {
    int error = 0;
    cJSON* jMessage = cJSON_Parse(json);
    if (jMessage) {
      const char* type = cJSON_GetStringValue(cJSON_GetObjectItem(jMessage, "type"));
      if (type && 0 == strcmp(type, "error")) {
        error = 1;
    		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_ERROR);
      }
      cJSON_Delete(jMessage);
    }
    if (!error) {
    		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_RESULTS);
    }
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s json payload: %s.\n", bugname ? bugname : "google_transcribe", json);
		if (!event) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "failed to create event subclass %s\n",
				error ? TRANSCRIBE_EVENT_ERROR : TRANSCRIBE_EVENT_RESULTS);
			return;
		}
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "google");
		switch_event_add_body(event, "%s", json);
	}
	if (!event) return;
	if (bugname) switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "media-bugname", bugname);
	switch_event_fire(&event);
}

static switch_bool_t capture_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	switch_core_session_t *session = switch_core_media_bug_get_session(bug);
	struct cap_cb* cb = (struct cap_cb*) switch_core_media_bug_get_user_data(bug);

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Got SWITCH_ABC_TYPE_INIT.\n");
			responseHandler(session, "start_of_transcript", cb->bugname);
		break;

	case SWITCH_ABC_TYPE_CLOSE:
		{
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Got SWITCH_ABC_TYPE_CLOSE, calling google_speech_session_cleanup.\n");
			responseHandler(session, "end_of_transcript", cb->bugname);
			google_speech_session_cleanup(session, 1, bug);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Finished SWITCH_ABC_TYPE_CLOSE.\n");
		}
		break;
	
	case SWITCH_ABC_TYPE_READ:

		return google_speech_frame(bug, user_data);
		break;

	case SWITCH_ABC_TYPE_WRITE:
	default:
		break;
	}

	return SWITCH_TRUE;
}

static switch_status_t transcribe_input_callback(switch_core_session_t *session, void *input, switch_input_type_t input_type, void *data, unsigned int len){
	if (input_type == SWITCH_INPUT_TYPE_EVENT) {
			switch_event_t *event;
			event = (switch_event_t *)input;
			if (event->event_id == SWITCH_EVENT_DETECTED_SPEECH) {
				return SWITCH_STATUS_BREAK;
			}
	}
	
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t do_stop(switch_core_session_t *session, char *bugname)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug = switch_channel_get_private(channel, bugname);

	if (bug) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Received user command command, calling google_speech_session_cleanup (possibly to stop prev transcribe)\n");
		status = google_speech_session_cleanup(session, 0, bug);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "stopped transcription.\n");
	}

	return status;
}

static switch_status_t start_capture2(switch_core_session_t *session, switch_media_bug_flag_t flags, 
  uint32_t sample_rate, char* lang, int interim, int single_utterance, int separate_recognition, int max_alternatives,
  int profinity_filter, int word_time_offset, int punctuation, const char* model, int enhanced, const char* hints, char* play_file)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug;
	switch_status_t status;
	switch_codec_implementation_t read_impl = { 0 };
	void *pUserData;
	uint32_t samples_per_second;
	switch_input_args_t args = { 0 };

	if (switch_channel_get_private(channel, MY_BUG_NAME)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "removing bug from previous transcribe\n");
		do_stop(session, MY_BUG_NAME);
	}

	/* returns FALSE and zeroes read_impl when no codec is negotiated yet (e.g.
	   an outbound leg ringing without early media) -- the strcasecmp below then
	   dereferenced a NULL iananame and crashed the FS process */
	if (switch_core_session_get_read_impl(session, &read_impl) != SWITCH_STATUS_SUCCESS || !read_impl.iananame) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"mod_google_transcribe: no read codec negotiated yet; try again after media is up\n");
		return SWITCH_STATUS_FALSE;
	}

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	samples_per_second = !strcasecmp(read_impl.iananame, "g722") ? read_impl.actual_samples_per_second : read_impl.samples_per_second;

	if (SWITCH_STATUS_FALSE == google_speech_session_init(session, responseHandler, sample_rate, samples_per_second, flags & SMBF_STEREO ? 2 : 1, lang, interim, MY_BUG_NAME, single_utterance,
	 separate_recognition, max_alternatives, profinity_filter, word_time_offset, punctuation, model, enhanced, hints, play_file, &pUserData)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error initializing google speech session.\n");
		return SWITCH_STATUS_FALSE;
	}
	if ((status = switch_core_media_bug_add(session, "google_transcribe", NULL, capture_callback, pUserData, 0, flags, &bug)) != SWITCH_STATUS_SUCCESS) {
		/* the gRPC read thread is already running out of the session pool;
		   without this teardown it outlives session destroy (use-after-free) */
		google_speech_session_orphan_cleanup(pUserData);
		return status;
	}

	switch_channel_set_private(channel, MY_BUG_NAME, bug);

	/* play the prompt, looking for detection result */
	if (play_file != NULL){
		args.input_callback = transcribe_input_callback;
		switch_ivr_play_file(session, NULL, play_file, &args);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t start_capture(switch_core_session_t *session, switch_media_bug_flag_t flags, 
  char* lang, int interim, char* bugname, int write_only)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug;
	switch_status_t status;
	switch_codec_implementation_t read_impl = { 0 };
	void *pUserData;
	uint32_t samples_per_second;
	int single_utterance = 0, separate_recognition = 0, max_alternatives = 0, profanity_filter = 0, word_time_offset = 0, punctuation = 0, enhanced = 0;
	const char* hints = NULL;
  const char* model = NULL;
	const char* var;

	if (strlen(bugname) > MAX_BUG_LEN) {
		/* the channel private is stored under the FULL name but cb->bugname is
		   a truncated copy used by the CLOSE-path stop -- a longer name made
		   cleanup miss the private and orphan the gRPC read thread */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"bugname too long (max %d chars): %s\n", MAX_BUG_LEN, bugname);
		return SWITCH_STATUS_FALSE;
	}

	if (switch_channel_get_private(channel, bugname)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "removing bug from previous transcribe\n");
		do_stop(session, bugname);
	}

	if (switch_true(switch_channel_get_variable(channel, "GOOGLE_SPEECH_SINGLE_UTTERANCE"))) {
      single_utterance = 1;
    }

	// transcribe each separately?
	if (switch_true(switch_channel_get_variable(channel, "GOOGLE_SPEECH_SEPARATE_RECOGNITION_PER_CHANNEL"))) {
      separate_recognition = 1;
    }

	// max alternatives
	if ((var = switch_channel_get_variable(channel, "GOOGLE_SPEECH_MAX_ALTERNATIVES"))) {
     	max_alternatives = atoi(var);
    }

	// profanity filter
	if (switch_true(switch_channel_get_variable(channel, "GOOGLE_SPEECH_PROFANITY_FILTER"))) {
      profanity_filter = 1;
    }

	// enable word offsets
	if (switch_true(switch_channel_get_variable(channel, "GOOGLE_SPEECH_ENABLE_WORD_TIME_OFFSETS"))) {
      word_time_offset = 1;
    }

	// enable automatic punctuation
	if (switch_true(switch_channel_get_variable(channel, "GOOGLE_SPEECH_ENABLE_AUTOMATIC_PUNCTUATION"))) {
      punctuation = 1;
    }

    // speech model
	if ((var = switch_channel_get_variable(channel, "GOOGLE_SPEECH_MODEL"))) {	
		model = var;    
	}

	// use enhanced model
	if (switch_true(switch_channel_get_variable(channel, "GOOGLE_SPEECH_USE_ENHANCED"))) {
		enhanced = 1;
	}

	// hints
	if ((var = switch_channel_get_variable(channel, "GOOGLE_SPEECH_HINTS"))) {
	  hints = var;
	}

	/* same guard as start_capture2: get_read_impl zeroes read_impl and returns
	   FALSE with no codec negotiated; the strcasecmp below dereferenced a NULL
	   iananame */
	if (switch_core_session_get_read_impl(session, &read_impl) != SWITCH_STATUS_SUCCESS || !read_impl.iananame) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"mod_google_transcribe: no read codec negotiated yet; try again after media is up\n");
		return SWITCH_STATUS_FALSE;
	}

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	samples_per_second = !strcasecmp(read_impl.iananame, "g722") ? read_impl.actual_samples_per_second : read_impl.samples_per_second;

	/* the write capture reads both directions but sends Google only the
	   second channel, so the recognizer is configured for one channel */
	if (SWITCH_STATUS_FALSE == google_speech_session_init(session, responseHandler, DEFAULT_SAMPLE_RATE, samples_per_second, (flags & SMBF_STEREO) && !write_only ? 2 : 1, lang, interim, bugname, single_utterance,
	 separate_recognition, max_alternatives, profanity_filter, word_time_offset, punctuation, model, enhanced, hints, NULL, &pUserData)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error initializing google speech session.\n");
		return SWITCH_STATUS_FALSE;
	}
	/* set before the bug is added, so before any frame reaches the callback */
	((struct cap_cb *) pUserData)->write_only = write_only;

	if ((status = switch_core_media_bug_add(session, bugname, NULL, capture_callback, pUserData, 0, flags, &bug)) != SWITCH_STATUS_SUCCESS) {
		/* the gRPC read thread is already running out of the session pool;
		   without this teardown it outlives session destroy (use-after-free) */
		google_speech_session_orphan_cleanup(pUserData);
		return status;
	}

	switch_channel_set_private(channel, bugname, bug);

	return SWITCH_STATUS_SUCCESS;
}

// #define TRANSCRIBE_API_SYNTAX "<uuid> [start|stop] [lang-code] [interim] [single-utterance](bool) [seperate-recognition](bool) [max-alternatives](int) [profinity-filter](bool) [word-time](bool) [punctuation](bool) [model](string) [enhanced](true) [hints](string without space) [play-file]"
#define TRANSCRIBE2_API_SYNTAX "<uuid> [start|stop] [lang-code] [interim]  [single-utterance] [seperate-recognition] [max-alternatives] [profinity-filter] [word-time] [punctuation] [sample-rate] [model] [enhanced] [hints] [play-file]"
SWITCH_STANDARD_API(transcribe2_function)
{
	char *mycmd = NULL, *argv[20] = { 0 };
	int argc = 0, enhanced = 0;
	uint32_t sample_rate = DEFAULT_SAMPLE_RATE;
	const char* hints = NULL;
	const char* model = NULL;
	char* play_file = NULL;
	
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_media_bug_flag_t flags = SMBF_READ_STREAM /* | SMBF_WRITE_STREAM | SMBF_READ_PING */;

	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
		argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (zstr(cmd) ||
      (argc < 2) || !argv[1] ||
      (!strcasecmp(argv[1], "start") && argc < 10) ||
      zstr(argv[0])) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error with command %s %s.\n",
			cmd ? cmd : "", argv[0] ? argv[0] : "");
		stream->write_function(stream, "-USAGE: %s\n", TRANSCRIBE2_API_SYNTAX);
		goto done;
	} else {
		switch_core_session_t *lsession = NULL;

		if ((lsession = switch_core_session_locate(argv[0]))) {
			if (!strcasecmp(argv[1], "stop")) {
    		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "stop transcribing\n");
				status = do_stop(lsession, MY_BUG_NAME);
			} else if (!strcasecmp(argv[1], "start")) {
        char* lang = argv[2];
        int interim = argc > 3 && !strcmp(argv[3], "true");
				int single_utterance =  !strcmp(argv[4], "true"); // single-utterance
				int separate_recognition = !strcmp(argv[5], "true"); // sepreate-recognition
				int max_alternatives = atoi(argv[6]); // max-alternatives
				int profinity_filter = !strcmp(argv[7], "true"); // profinity-filter
				int word_time_offset = !strcmp(argv[8], "true"); // word-time
				int punctuation      = !strcmp(argv[9], "true");  //punctuation
				if (argc > 10) {
					sample_rate = atol(argv[10]);
					/* atol garbage is 0, and the value becomes speex's output rate:
					   0 (or an absurd rate) is a resampler misconfig at best */
					if (sample_rate < 4000 || sample_rate > 48000) {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
							"invalid sample-rate %s (need 4000..48000)\n", argv[10]);
						switch_core_session_rwunlock(lsession);
						goto done;
					}
				}
				/* model (argv[11]) and enhanced (argv[12]) gated separately: the
			   combined argc > 12 gate silently ignored a model supplied as the
			   12th argument without an enhanced flag after it */
				if (argc > 11){
					model = argv[11]; // model
				}
				if (argc > 12){
					enhanced = !strcmp(argv[12], "true"); // enhanced
				}
				if (argc > 13){
					hints = argv[13]; // hints
				}
				if (argc > 14){
					play_file = argv[14];
				}
    		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "start transcribing %s %s\n", lang, interim ? "interim": "complete");
				status = start_capture2(lsession, flags, sample_rate, lang, interim, single_utterance, separate_recognition,max_alternatives,
				profinity_filter, word_time_offset, punctuation, model, enhanced, hints, play_file);
			}
			switch_core_session_rwunlock(lsession);
		}
	}

	if (status == SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "+OK Success\n");
	} else {
		stream->write_function(stream, "-ERR Operation Failed\n");
	}

  done:

	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

#define TRANSCRIBE_API_SYNTAX "<uuid> [start|stop] [lang-code] [interim|full] [stereo|mono|write] [bug-name]"
SWITCH_STANDARD_API(transcribe_function)
{
	char *mycmd = NULL, *argv[6] = { 0 };
	int argc = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_media_bug_flag_t flags = SMBF_READ_STREAM /* | SMBF_WRITE_STREAM | SMBF_READ_PING */;

	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
		argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (zstr(cmd) ||
      (argc < 2) || !argv[1] ||
      (!strcasecmp(argv[1], "stop") && argc < 2) ||
      (!strcasecmp(argv[1], "start") && argc < 3) ||
      zstr(argv[0])) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error with command %s %s %s.\n",
			cmd ? cmd : "", argv[0] ? argv[0] : "", argv[1] ? argv[1] : "");
		stream->write_function(stream, "-USAGE: %s\n", TRANSCRIBE_API_SYNTAX);
		goto done;
	} else {
		switch_core_session_t *lsession = NULL;

		if ((lsession = switch_core_session_locate(argv[0]))) {
			if (!strcasecmp(argv[1], "stop")) {
				char *bugname = argc > 2 ? argv[2] : MY_BUG_NAME;
    		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "stop transcribing\n");
				status = do_stop(lsession, bugname);
			} else if (!strcasecmp(argv[1], "start")) {
        char* lang = argv[2];
        int interim = argc > 3 && !strcmp(argv[3], "interim");
				char *bugname = argc > 5 ? argv[5] : MY_BUG_NAME;
				int write_only = 0;
				if (argc > 4 && !strcmp(argv[4], "stereo")) {
          flags |= SMBF_WRITE_STREAM ;
          flags |= SMBF_STEREO;
				}
				/* write: transcribe only the audio the channel sends. Captured as
				   stereo, not as a write-only bug; the frame callback reads such
				   bugs with fill=SWITCH_FALSE so the silent direction is
				   zero-filled and frames keep flowing on one-sided media
				   (fill=SWITCH_TRUE returns no frame unless BOTH directions have
				   a full frame buffered). Note the callback is read-driven: if
				   the caller's RTP stops entirely, no frames arrive regardless. */
				else if (argc > 4 && !strcmp(argv[4], "write")) {
          flags |= SMBF_WRITE_STREAM ;
          flags |= SMBF_STEREO;
          write_only = 1;
				}
    		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "%s start transcribing %s %s%s\n", bugname, lang, interim ? "interim": "complete", write_only ? " (write side)" : "");
				status = start_capture(lsession, flags, lang, interim, bugname, write_only);
			}
			switch_core_session_rwunlock(lsession);
		}
	}

	if (status == SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "+OK Success\n");
	} else {
		stream->write_function(stream, "-ERR Operation Failed\n");
	}

  done:

	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_transcribe_load)
{
	switch_api_interface_t *api_interface;

	/* create/register custom event message types.
	   NB: TRANSCRIBE_EVENT_ERROR (jambonz_transcribe::error) is deliberately
	   shared with the other transcribe modules (consumers key on it), so a
	   sibling may already have reserved it -- INUSE is success, and only the
	   module that actually reserved a name frees it at unload. The glue fires
	   the error subclass (google_glue's in-band and Finish()-status paths) but
	   it was never reserved here; add it. */
	{
		static const char* subclasses[] = {
			TRANSCRIBE_EVENT_RESULTS,
			TRANSCRIBE_EVENT_END_OF_UTTERANCE,
			TRANSCRIBE_EVENT_START_OF_TRANSCRIPT,
			TRANSCRIBE_EVENT_END_OF_TRANSCRIPT,
			TRANSCRIBE_EVENT_NO_AUDIO_DETECTED,
			TRANSCRIBE_EVENT_MAX_DURATION_EXCEEDED,
			TRANSCRIBE_EVENT_PLAY_INTERRUPT,
			TRANSCRIBE_EVENT_VAD_DETECTED,
			TRANSCRIBE_EVENT_ERROR
		};
		size_t i;
		if (sizeof(subclasses) / sizeof(subclasses[0]) != N_RESERVED_SUBCLASSES) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "subclass table size mismatch\n");
			return SWITCH_STATUS_TERM;
		}
		for (i = 0; i < sizeof(subclasses) / sizeof(subclasses[0]); i++) {
			switch_status_t st = switch_event_reserve_subclass(subclasses[i]);
			if (st == SWITCH_STATUS_INUSE) {
				g_reserved_subclasses[i] = 0;
				continue;
			}
			if (st != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", subclasses[i]);
				/* FreeSWITCH does not call the shutdown hook for a failed load:
				   anything already reserved stays reserved for the life of the
				   process. Release what we took (only what WE took). */
				while (i-- > 0) {
					if (g_reserved_subclasses[i]) switch_event_free_subclass(subclasses[i]);
					g_reserved_subclasses[i] = 0;
				}
				return SWITCH_STATUS_TERM;
			}
			g_reserved_subclasses[i] = 1;
		}
	}

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Google Speech Transcription API loading..\n");

  if (SWITCH_STATUS_FALSE == google_speech_init()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Failed initializing google speech interface\n");
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Google Speech Transcription API successfully loaded\n");

	SWITCH_ADD_API(api_interface, "uuid_google_transcribe", "Google Speech Transcription API", transcribe_function, TRANSCRIBE_API_SYNTAX);
	SWITCH_ADD_API(api_interface, "uuid_google_transcribe2", "Google Speech Transcription API", transcribe2_function, TRANSCRIBE2_API_SYNTAX);
	switch_console_set_complete("add uuid_google_transcribe start lang-code");
	switch_console_set_complete("add uuid_google_transcribe stop ");

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

/*
  Called when the system shuts down
  Macro expands to: switch_status_t mod_google_transcribe_shutdown() */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_transcribe_shutdown)
{
	/* refuse to unload with live sessions: the gRPC read threads run module
	   code out of session pools, so unloading unmaps module text under them.
	   Same gate as azure/aws (v0.7.0). */
	int active = google_speech_sessions_active();
	if (active > 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"mod_google_transcribe: %d session(s) still active; not unloading. Stop them first (uuid_google_transcribe <uuid> stop) or hang up the calls.\n", active);
		return SWITCH_STATUS_FALSE;
	}

	google_speech_cleanup();
	{
		/* free only the subclasses this module reserved at load (the shared
		   error name may belong to a sibling) */
		static const char* subclasses[] = {
			TRANSCRIBE_EVENT_RESULTS,
			TRANSCRIBE_EVENT_END_OF_UTTERANCE,
			TRANSCRIBE_EVENT_START_OF_TRANSCRIPT,
			TRANSCRIBE_EVENT_END_OF_TRANSCRIPT,
			TRANSCRIBE_EVENT_NO_AUDIO_DETECTED,
			TRANSCRIBE_EVENT_MAX_DURATION_EXCEEDED,
			TRANSCRIBE_EVENT_PLAY_INTERRUPT,
			TRANSCRIBE_EVENT_VAD_DETECTED,
			TRANSCRIBE_EVENT_ERROR
		};
		size_t i;
		for (i = 0; i < sizeof(subclasses) / sizeof(subclasses[0]); i++) {
			if (g_reserved_subclasses[i]) {
				switch_event_free_subclass(subclasses[i]);
				g_reserved_subclasses[i] = 0;
			}
		}
	}
	return SWITCH_STATUS_SUCCESS;
}

