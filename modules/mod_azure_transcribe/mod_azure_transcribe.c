/* 
 *
 * mod_azure_transcribe.c -- Freeswitch module for using azure streaming transcribe api
 *
 */
#include "mod_azure_transcribe.h"
#include "azure_transcribe_glue.h"

/* Prototypes */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_azure_transcribe_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_azure_transcribe_load);

SWITCH_MODULE_DEFINITION(mod_azure_transcribe, mod_azure_transcribe_load, mod_azure_transcribe_shutdown, NULL);

static switch_status_t do_stop(switch_core_session_t *session, char* bugname);

static void responseHandler(switch_core_session_t* session, const char* eventName, const char * json, const char* bugname, int finished) {
	switch_event_t *event = NULL;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	/* json is NULL for the bodiless events (vad_detected, utterance boundaries);
	   %s on NULL is UB (glibc prints "(null)", but don't rely on it) */
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "responseHandler event %s, body %s.\n", eventName, json ? json : "(none)");
	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, eventName) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "responseHandler failed to create event subclass %s\n", eventName);
		return;
	}
	switch_channel_event_set_data(channel, event);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "microsoft");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-session-finished", finished ? "true" : "false");
	if (finished) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "responseHandler returning event %s, from finished recognition session\n", eventName);
	}
	if (json) switch_event_add_body(event, "%s", json);
	if (bugname) switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "media-bugname", bugname);
	switch_event_fire(&event);
}

static switch_bool_t capture_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	/* (no session local: no arm needs it) */
	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Got SWITCH_ABC_TYPE_INIT.\n");
		break;

	case SWITCH_ABC_TYPE_CLOSE:
		{
			struct cap_cb* cb = (struct cap_cb*) switch_core_media_bug_get_user_data(bug);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Got SWITCH_ABC_TYPE_CLOSE.\n");

			/* stop THIS bug's recognizer, not whatever the (possibly
			   overwritten) channel-private resolves to by name */
			azure_transcribe_session_close(cb);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Finished SWITCH_ABC_TYPE_CLOSE.\n");
		}
		break;
	
	case SWITCH_ABC_TYPE_READ:

		return azure_transcribe_frame(bug, user_data);
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
	   an outbound leg ringing without early media) -- a strcasecmp on the NULL
	   iananame here used to crash the FS process. google and aws had the guard
	   in their glue (55ad536); it was missing here and at the glue boundary. */
	if (switch_core_session_get_read_impl(session, &read_impl) != SWITCH_STATUS_SUCCESS || !read_impl.iananame) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"mod_azure_transcribe: no read codec negotiated yet; try again after media is up\n");
		return SWITCH_STATUS_FALSE;
	}

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	/* the glue re-derives the rate from the codec; the g722-aware value
	   computed here fed a parameter the glue never used */
	if (SWITCH_STATUS_FALSE == azure_transcribe_session_init(session, responseHandler,
		flags & SMBF_STEREO ? 2 : 1, lang, interim, bugname, &pUserData)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error initializing azure speech session.\n");
		return SWITCH_STATUS_FALSE;
	}
	if ((status = switch_core_media_bug_add(session, bugname, NULL, capture_callback, pUserData, 0, flags, &bug)) != SWITCH_STATUS_SUCCESS) {
		/* session_init already created the recognizer and (non-VAD) started
		   continuous recognition; without teardown it leaked and kept firing
		   events for a start that reported failure */
		azure_transcribe_session_cleanup(pUserData);
		return status;
	}
  switch_channel_set_private(channel, bugname, bug);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "added media bug for azure transcribe\n");

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t do_stop(switch_core_session_t *session, char* bugname)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug = switch_channel_get_private(channel, bugname);

	if (bug) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "do_stop: Received user command command to stop transcribe.\n");
		status = azure_transcribe_session_stop(session, 0, bugname);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "do_stop: stopped transcribe.\n");
	}

	return status;
}

#define TRANSCRIBE_API_SYNTAX "<uuid> [start|stop] lang-code [interim] [stereo|mono] [bugname]"
SWITCH_STANDARD_API(azure_transcribe_function)
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
    		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "stop transcribing %s\n", bugname);
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
					   a longer name would make that stop miss the private and never
					   reap the recognizer */
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


SWITCH_MODULE_LOAD_FUNCTION(mod_azure_transcribe_load)
{
	switch_api_interface_t *api_interface;

	/* create/register custom event message types.
	   NB: the ERROR subclass NAME (inherited jambonz_transcribe::error) is frozen --
	   external consumers key on it. */
	{
		static const char* subclasses[] = {
			TRANSCRIBE_EVENT_RESULTS,
			TRANSCRIBE_EVENT_START_OF_UTTERANCE,
			TRANSCRIBE_EVENT_END_OF_UTTERANCE,
			TRANSCRIBE_EVENT_NO_SPEECH_DETECTED,
			TRANSCRIBE_EVENT_VAD_DETECTED,
			/* the glue fires the error subclass (notifyWriteFailure, onCanceled,
			   unexpected SessionStopped) but it was never reserved -- aws was
			   fixed for exactly this (b8be43a) and azure was not */
			TRANSCRIBE_EVENT_ERROR
		};
		size_t i;
		for (i = 0; i < sizeof(subclasses) / sizeof(subclasses[0]); i++) {
			if (switch_event_reserve_subclass(subclasses[i]) != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", subclasses[i]);
				/* FreeSWITCH does not call the shutdown hook for a failed load:
				   anything already reserved stays reserved for the life of the
				   process, and every subsequent load fails at this same spot
				   until FS restarts. Release what we took. */
				while (i-- > 0) {
					switch_event_free_subclass(subclasses[i]);
				}
				return SWITCH_STATUS_TERM;
			}
		}
	}

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "azure Speech Transcription API loading..\n");

  if (SWITCH_STATUS_FALSE == azure_transcribe_init()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Failed initializing azure speech interface\n");
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "azure Speech Transcription API successfully loaded\n");

	SWITCH_ADD_API(api_interface, "uuid_azure_transcribe", "azure Speech Transcription API", azure_transcribe_function, TRANSCRIBE_API_SYNTAX);
	switch_console_set_complete("add uuid_azure_transcribe start lang-code [interim|final] [stereo|mono] [bugname]");
	switch_console_set_complete("add uuid_azure_transcribe stop ");

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

/*
  Called when the system shuts down
  Macro expands to: switch_status_t mod_azure_transcribe_shutdown() */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_azure_transcribe_shutdown)
{
	int active = azure_transcribe_active_sessions();
	if (active > 0) {
		/* Refuse the unload: each active recognizer's SDK callback threads and
		   the detached reaper threads execute module code, which becomes
		   unmapped text the moment the .so is unloaded -- a guaranteed crash.
		   FS keeps the module loaded when shutdown returns FALSE. */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"mod_azure_transcribe: %d transcription session(s) still active; refusing unload -- stop transcriptions (or hang up the calls) first\n",
			active);
		return SWITCH_STATUS_FALSE;
	}
	azure_transcribe_cleanup();
	switch_event_free_subclass(TRANSCRIBE_EVENT_RESULTS);
	switch_event_free_subclass(TRANSCRIBE_EVENT_START_OF_UTTERANCE);
	switch_event_free_subclass(TRANSCRIBE_EVENT_END_OF_UTTERANCE);
	switch_event_free_subclass(TRANSCRIBE_EVENT_NO_SPEECH_DETECTED);
	switch_event_free_subclass(TRANSCRIBE_EVENT_VAD_DETECTED);
	switch_event_free_subclass(TRANSCRIBE_EVENT_ERROR);
	return SWITCH_STATUS_SUCCESS;
}
