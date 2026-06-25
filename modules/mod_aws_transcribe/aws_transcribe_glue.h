#ifndef __AWS_GLUE_H__
#define __AWS_GLUE_H__

switch_status_t aws_transcribe_init();
switch_status_t aws_transcribe_cleanup();
switch_status_t aws_transcribe_session_init(switch_core_session_t *session, responseHandler_t responseHandler, 
		uint32_t samples_per_second, uint32_t channels, char* lang, int interim, char *bugname, void **ppUserData);
switch_status_t aws_transcribe_session_stop(switch_core_session_t *session, int channelIsClosing, char* bugname);
/* tear down a cap_cb whose worker thread/GStreamer were created by
   aws_transcribe_session_init but which was never attached to a media bug
   (e.g. switch_core_media_bug_add failed). Stops+joins the thread and frees. */
void aws_transcribe_session_cleanup(void *pUserData);
switch_bool_t aws_transcribe_frame(switch_media_bug_t *bug, void* user_data);

#endif