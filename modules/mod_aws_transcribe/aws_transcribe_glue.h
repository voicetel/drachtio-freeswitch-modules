#ifndef __AWS_GLUE_H__
#define __AWS_GLUE_H__

switch_status_t aws_transcribe_init();
switch_status_t aws_transcribe_cleanup();
switch_status_t aws_transcribe_session_init(switch_core_session_t *session, responseHandler_t responseHandler,
		uint32_t channels, char* lang, int interim, char *bugname, void **ppUserData);
switch_status_t aws_transcribe_session_stop(switch_core_session_t *session, int channelIsClosing, char* bugname);
/* teardown for a media bug's own CLOSE callback: stops THIS bug's session
   (finish + join + killcb) without resolving anything through the channel
   private -- safe under concurrent same-bugname starts. (This header is
   always included after mod_aws_transcribe.h, which defines struct cap_cb.) */
switch_status_t aws_transcribe_session_close(struct cap_cb *cb);
/* tear down a cap_cb whose worker thread/GStreamer were created by
   aws_transcribe_session_init but which was never attached to a media bug
   (e.g. switch_core_media_bug_add failed). Stops+joins the thread and frees. */
void aws_transcribe_session_cleanup(void *pUserData);
switch_bool_t aws_transcribe_frame(switch_media_bug_t *bug, void* user_data);
/* live transcription session count; > 0 means unload must be refused */
int aws_transcribe_active_sessions();
/* shutdown-hook entry: stops accepting new sessions and returns the live
   count atomically with that decision */
int aws_transcribe_shutdown_begin();

#endif