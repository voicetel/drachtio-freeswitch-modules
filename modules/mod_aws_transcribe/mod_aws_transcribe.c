/* 
 *
 * mod_aws_transcribe.c -- Freeswitch module for using aws streaming transcribe api
 *
 */
#include "mod_aws_transcribe.h"
#include "aws_transcribe_glue.h"

/* Prototypes */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_aws_transcribe_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_aws_transcribe_load);

SWITCH_MODULE_DEFINITION(mod_aws_transcribe, mod_aws_transcribe_load, mod_aws_transcribe_shutdown, NULL);

static switch_status_t do_stop(switch_core_session_t *session, char* bugname);

static void responseHandler(switch_core_session_t* session, const char * json, const char* bugname) {
	switch_event_t *event = NULL;
	switch_channel_t *channel = switch_core_session_get_channel(session);

	if (!json) return;
	if (0 == strcmp("vad_detected", json)) {
		if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_VAD_DETECTED) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "responseHandler: failed to create subclass %s\n", TRANSCRIBE_EVENT_VAD_DETECTED);
			return;
		}
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "aws");
	}
	else if (0 == strcmp("end_of_transcript", json)) {
		if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_END_OF_TRANSCRIPT) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "responseHandler: failed to create subclass %s\n", TRANSCRIBE_EVENT_END_OF_TRANSCRIPT);
			return;
		}
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "aws");
	}
	else if (0 == strcmp("max_duration_exceeded", json)) {
		if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_MAX_DURATION_EXCEEDED) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "responseHandler: failed to create subclass %s\n", TRANSCRIBE_EVENT_MAX_DURATION_EXCEEDED);
			return;
		}
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "aws");
	}
	else if (0 == strcmp("no_audio", json)) {
		if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_NO_AUDIO_DETECTED) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "responseHandler: failed to create subclass %s\n", TRANSCRIBE_EVENT_NO_AUDIO_DETECTED);
			return;
		}
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "aws");
	}
	else {
    int error = 0;
    cJSON* jMessage = cJSON_Parse(json);
    if (jMessage) {
      const char* type = cJSON_GetStringValue(cJSON_GetObjectItem(jMessage, "type"));
      if (type && 0 == strcmp(type, "error")) {
        error = 1;
    		if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_ERROR) != SWITCH_STATUS_SUCCESS) {
    			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "responseHandler: failed to create subclass %s\n", TRANSCRIBE_EVENT_ERROR);
    			cJSON_Delete(jMessage);
    			return;
    		}
      }
      cJSON_Delete(jMessage);
    }
    if (!error) {
    		if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_RESULTS) != SWITCH_STATUS_SUCCESS) {
    			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "responseHandler: failed to create subclass %s\n", TRANSCRIBE_EVENT_RESULTS);
    			return;
    		}
    }
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "json payload: %s.\n", json);
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "aws");
		switch_event_add_body(event, "%s", json);
	}
	if (!event) return;
	if (bugname) switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "media-bugname", bugname);
	switch_event_fire(&event);
}


static switch_bool_t capture_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	/* (no session local: the CLOSE arm stops the bug's own cb directly) */
	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Got SWITCH_ABC_TYPE_INIT.\n");
		break;

	case SWITCH_ABC_TYPE_CLOSE:
		{
			struct cap_cb* cb = (struct cap_cb*) switch_core_media_bug_get_user_data(bug);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Got SWITCH_ABC_TYPE_CLOSE.\n");

			/* stop THIS bug's session, not whatever the (possibly overwritten)
			   channel-private resolves to by name */
			aws_transcribe_session_close(cb);
			//switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Finished SWITCH_ABC_TYPE_CLOSE.\n");
		}
		break;
	
	case SWITCH_ABC_TYPE_READ:

		return aws_transcribe_frame(bug, user_data);
		break;

	case SWITCH_ABC_TYPE_WRITE:
	default:
		break;
	}

	return SWITCH_TRUE;
}

static switch_status_t start_capture(switch_core_session_t *session, switch_media_bug_flag_t flags, 
  char* lang, int interim, char* bugname)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug;
	switch_status_t status;
	switch_codec_implementation_t read_impl = { 0 };
	void *pUserData;

	if (switch_channel_get_private(channel, bugname)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "removing bug from previous transcribe\n");
		do_stop(session, bugname);
	}

	/* returns FALSE and zeroes read_impl when no codec is negotiated yet (e.g.
	   an outbound leg ringing without early media); the glue re-derives the
	   rate from the codec itself, so this is purely a presence check (a NULL
	   iananame used to be strcasecmp'd here -- a whole-process crash) */
	if (switch_core_session_get_read_impl(session, &read_impl) != SWITCH_STATUS_SUCCESS || !read_impl.iananame) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"mod_aws_transcribe: no read codec negotiated yet; try again after media is up\n");
		return SWITCH_STATUS_FALSE;
	}

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if (SWITCH_STATUS_FALSE == aws_transcribe_session_init(session, responseHandler, flags & SMBF_STEREO ? 2 : 1, lang, interim, bugname, &pUserData)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error initializing aws speech session.\n");
		return SWITCH_STATUS_FALSE;
	}
	if ((status = switch_core_media_bug_add(session, bugname, NULL, capture_callback, pUserData, 0, flags, &bug)) != SWITCH_STATUS_SUCCESS) {
		/* The bug was never attached, so the channel-private was never set and
		   do_stop/session_stop can't find the cb. session_init already created the
		   worker thread + GStreamer, so tear them down here to avoid leaking. */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error adding media bug for aws transcribe; cleaning up session.\n");
		aws_transcribe_session_cleanup(pUserData);
		return status;
	}
  switch_channel_set_private(channel, bugname, bug);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "added media bug for aws transcribe\n");

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t do_stop(switch_core_session_t *session, char* bugname)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug = switch_channel_get_private(channel, bugname);

	if (bug) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Received user command to stop transcribe on %s.\n", bugname);
		status = aws_transcribe_session_stop(session, 0, bugname);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "stopped transcribe.\n");
	}

	return status;
}

#define TRANSCRIBE_API_SYNTAX "<uuid> [start|stop] lang-code [interim] [stereo|mono] [bugname]"
SWITCH_STANDARD_API(aws_transcribe_function)
{
	char *mycmd = NULL, *argv[6] = { 0 };
	int argc = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_media_bug_flag_t flags = SMBF_READ_STREAM /* | SMBF_WRITE_STREAM | SMBF_READ_PING */;

	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
		argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (zstr(cmd) ||
      argc < 2 || zstr(argv[1]) ||
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
        int interim = 0;
				char *bugname = MY_BUG_NAME;
				int i;
				/* the options were previously positional: "start en stereo" put
				   "stereo" in the interim slot and silently ran mono, and the
				   console completion advertised a "final" token the parser did
				   not know. Accept the keywords in any order; the first
				   non-keyword token is the bugname. */
				for (i = 3; i < argc; i++) {
          if (!strcasecmp(argv[i], "interim")) {
            interim = 1;
          }
          else if (!strcasecmp(argv[i], "stereo")) {
            flags |= SMBF_WRITE_STREAM;
            flags |= SMBF_STEREO;
          }
          else if (!strcasecmp(argv[i], "mono") || !strcasecmp(argv[i], "final")) {
            /* defaults; accepted for symmetry with the console completion */
          }
          else {
            bugname = argv[i];
          }
				}
				if (strlen(bugname) >= MAX_BUG_LEN) {
					/* the channel private is stored under the FULL name but
					   cb->bugname is a truncated copy used by the hangup CLOSE path;
					   a longer name would make that stop miss the private and leak
					   the worker thread + streamer past session destroy */
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						"bugname too long (max %d chars): %s\n", MAX_BUG_LEN - 1, bugname);
					status = SWITCH_STATUS_FALSE;
				}
				else {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "start transcribing %s %s %s\n", lang, interim ? "interim": "complete", bugname);
					status = start_capture(lsession, flags, lang, interim, bugname);
				}
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


SWITCH_MODULE_LOAD_FUNCTION(mod_aws_transcribe_load)
{
	switch_api_interface_t *api_interface;

	/* create/register custom event message types.
	   NB: the ERROR subclass NAME (inherited jambonz_transcribe::error) is frozen --
	   external consumers key on it. */
	{
		static const char* subclasses[] = {
			TRANSCRIBE_EVENT_RESULTS,
			TRANSCRIBE_EVENT_END_OF_TRANSCRIPT,
			TRANSCRIBE_EVENT_NO_AUDIO_DETECTED,
			TRANSCRIBE_EVENT_MAX_DURATION_EXCEEDED,
			TRANSCRIBE_EVENT_VAD_DETECTED,
			/* the error name is frozen (consumers key on it); this only adds the
			   missing reservation the other five subclasses already had */
			TRANSCRIBE_EVENT_ERROR
		};
		size_t i;
		for (i = 0; i < sizeof(subclasses) / sizeof(subclasses[0]); i++) {
			if (switch_event_reserve_subclass(subclasses[i]) != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", subclasses[i]);
				/* FreeSWITCH does not call the shutdown hook for a failed load:
				   anything already reserved stays reserved for the life of the
				   process, and every subsequent 'load mod_aws_transcribe' fails at
				   this same spot until FS restarts. Release what we took. */
				while (i-- > 0) {
					switch_event_free_subclass(subclasses[i]);
				}
				return SWITCH_STATUS_TERM;
			}
		}
	}

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "AWS Speech Transcription API loading..\n");

  /* aws_transcribe_init cannot fail (Aws::InitAPI has no failure return here),
     so the old FAILURE branch was dead */
  aws_transcribe_init();

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "AWS Speech Transcription API successfully loaded\n");

	SWITCH_ADD_API(api_interface, "uuid_aws_transcribe", "AWS Speech Transcription API", aws_transcribe_function, TRANSCRIBE_API_SYNTAX);
	switch_console_set_complete("add uuid_aws_transcribe start lang-code [interim|final] [stereo|mono]");
	switch_console_set_complete("add uuid_aws_transcribe stop ");

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

/*
  Called when the system shuts down
  Macro expands to: switch_status_t mod_aws_transcribe_shutdown() */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_aws_transcribe_shutdown)
{
	/* stop accepting new sessions and read the count atomically with that
	   decision, so a start racing the unload is either counted or refused */
	int active = aws_transcribe_shutdown_begin();
	if (active > 0) {
		/* Refuse the unload: the per-session worker threads still own live
		   TranscribeStreamingServiceClients, and Aws::ShutdownAPI below would
		   run the SDK's global teardown (curl/exeuctor globals) under them --
		   undefined behavior / crash. FS keeps the module loaded when shutdown
		   returns FALSE. */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"mod_aws_transcribe: %d transcription session(s) still active; refusing unload -- stop transcriptions (or hang up the calls) first\n",
			active);
		return SWITCH_STATUS_FALSE;
	}
	aws_transcribe_cleanup();
	switch_event_free_subclass(TRANSCRIBE_EVENT_RESULTS);
	switch_event_free_subclass(TRANSCRIBE_EVENT_END_OF_TRANSCRIPT);
	switch_event_free_subclass(TRANSCRIBE_EVENT_NO_AUDIO_DETECTED);
	switch_event_free_subclass(TRANSCRIBE_EVENT_MAX_DURATION_EXCEEDED);
	switch_event_free_subclass(TRANSCRIBE_EVENT_VAD_DETECTED);
	switch_event_free_subclass(TRANSCRIBE_EVENT_ERROR);
	return SWITCH_STATUS_SUCCESS;
}
