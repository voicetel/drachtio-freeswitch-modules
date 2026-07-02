#include <switch.h>
#include <switch_json.h>
#include <string.h>
#include <string>
#include <mutex>
#include <thread>
#include <list>
#include <algorithm>
#include <functional>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <regex>
#include <iostream>
#include <unordered_map>

#include "mod_deepgram_transcribe.h"
#include "simple_buffer.h"
#include "parser.hpp"
#include "audio_pipe.hpp"

#define RTP_PACKETIZATION_PERIOD 20
#define FRAME_SIZE_8000  320 /*which means each 20ms frame as 320 bytes at 8 khz (1 channel only)*/

namespace {
  /* RAII guard for switch_core_session_locate / switch_core_session_rwunlock.
     Locates on construction; rwunlocks on destruction so every exit path releases
     the read lock. Behavior is identical to the manual locate/unlock pattern. */
  struct SessionLock {
    switch_core_session_t* session;
    explicit SessionLock(const char* uuid) : session(switch_core_session_locate(uuid)) {}
    ~SessionLock() { if (session) switch_core_session_rwunlock(session); }
    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;
  };

  /* RAII unlock guard for switch_mutex_t. It does NOT acquire the lock: it adopts
     an already-held mutex (e.g. one taken via switch_mutex_trylock) and unlocks it
     on every exit path so a locked region cannot leak the mutex on an early return.
     Trylock semantics at the call site are preserved exactly. */
  struct MutexUnlockGuard {
    switch_mutex_t* mutex;
    explicit MutexUnlockGuard(switch_mutex_t* m) : mutex(m) {}
    ~MutexUnlockGuard() { switch_mutex_unlock(mutex); }
    MutexUnlockGuard(const MutexUnlockGuard&) = delete;
    MutexUnlockGuard& operator=(const MutexUnlockGuard&) = delete;
  };

  static bool hasDefaultCredentials = false;
  static const char* defaultApiKey = nullptr;
  static const char *requestedBufferSecs = std::getenv("MOD_AUDIO_FORK_BUFFER_SECS");
  static int nAudioBufferSecs = std::max(1, std::min(requestedBufferSecs ? ::atoi(requestedBufferSecs) : 2, 5));
  static const char *requestedNumServiceThreads = std::getenv("MOD_AUDIO_FORK_SERVICE_THREADS");
  static unsigned int nServiceThreads = std::max(1, std::min(requestedNumServiceThreads ? ::atoi(requestedNumServiceThreads) : 1, 5));
  static std::atomic<unsigned int> idxCallCount{0};
  static uint32_t playCount = 0;

  /* deepgram model / tier defaults by language */
  struct LanguageInfo {
      std::string tier;
      std::string model;
  };

  static const std::unordered_map<std::string, LanguageInfo> languageLookupTable = {
      {"zh", {"base", "general"}},
      {"zh-CN", {"base", "general"}},
      {"zh-TW", {"base", "general"}},
      {"da", {"enhanced", "general"}},
      {"en", {"nova", "phonecall"}},
      {"en-US", {"nova", "phonecall"}},
      {"en-AU", {"nova", "general"}},
      {"en-GB", {"nova", "general"}},
      {"en-IN", {"nova", "general"}},
      {"en-NZ", {"nova", "general"}},
      {"nl", {"enhanced", "general"}},
      {"fr", {"enhanced", "general"}},
      {"fr-CA", {"base", "general"}},
      {"de", {"enhanced", "general"}},
      {"hi", {"enhanced", "general"}},
      {"hi-Latn", {"base", "general"}},
      {"id", {"base", "general"}},
      {"ja", {"enhanced", "general"}},
      {"ko", {"enhanced", "general"}},
      {"no", {"enhanced", "general"}},
      {"pl", {"enhanced", "general"}},
      {"pt", {"enhanced", "general"}},
      {"pt-BR", {"enhanced", "general"}},
      {"pt-PT", {"enhanced", "general"}},
      {"ru", {"base", "general"}},
      {"es", {"nova", "general"}},
      {"es-419", {"nova", "general"}},
      {"sv", {"enhanced", "general"}},
      {"ta", {"enhanced", "general"}},
      {"tr", {"base", "general"}},
      {"uk", {"base", "general"}}
  };

  static bool getLanguageInfo(const std::string& language, LanguageInfo& info) {
      auto it = languageLookupTable.find(language);
      if (it != languageLookupTable.end()) {
          info = it->second;
          return true;
      }
      return false;
  }

  static const char* emptyTranscript = "{\"alternatives\":[{\"transcript\":\"\",\"confidence\":0.0,\"words\":[]}]}";

  // Hand the AudioPipe off to a detached thread that closes the connection,
  // waits for the lws CLOSED to be signalled, then deletes it (via the
  // shared_ptr destructor). Must be called with tech_pvt->mutex held and only
  // after the media bug has been removed, so no media-bug thread can still touch
  // the pipe. sessionId/id are captured BY VALUE so the thread never dereferences
  // tech_pvt after this returns (tech_pvt lives in the session pool and may be
  // freed once the session tears down).
  static void reaper(private_t *tech_pvt) {
    std::shared_ptr<deepgram::AudioPipe> pAp;
    pAp.reset((deepgram::AudioPipe *)tech_pvt->pAudioPipe);
    tech_pvt->pAudioPipe = nullptr;

    std::string sessionId(tech_pvt->sessionId);
    uint32_t id = tech_pvt->id;
    std::thread t([pAp, sessionId, id]{
      pAp->finish();
      pAp->waitForClose();
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s (%u) got remote close\n", sessionId.c_str(), id);
    });
    t.detach();
  }

  static void destroy_tech_pvt(private_t *tech_pvt) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s (%u) destroy_tech_pvt\n", tech_pvt->sessionId, tech_pvt->id);
    if (tech_pvt) {
      if (tech_pvt->pAudioPipe) {
        deepgram::AudioPipe* p = (deepgram::AudioPipe *) tech_pvt->pAudioPipe;
        delete p;
        tech_pvt->pAudioPipe = nullptr;
      }
      if (tech_pvt->resampler) {
          speex_resampler_destroy(tech_pvt->resampler);
          tech_pvt->resampler = NULL;
      }

      /*
      if (tech_pvt->vad) {
        switch_vad_destroy(&tech_pvt->vad);
        tech_pvt->vad = nullptr;
      }
      */
    }
  }

  std::string encodeURIComponent(std::string decoded)
  {

      std::ostringstream oss;
      std::regex r("[!'\\(\\)*-.0-9A-Za-z_~:]");

      for (char &c : decoded)
      {
          if (std::regex_match((std::string){c}, r))
          {
              oss << c;
          }
          else
          {
              oss << "%" << std::uppercase << std::hex << (0xff & c);
          }
      }
      return oss.str();
  }

  std::string& constructPath(switch_core_session_t* session, std::string& path, 
    int sampleRate, int channels, const char* language, int interim) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    const char *var ;
    const char *model = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_MODEL");
    const char *customModel = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_CUSTOM_MODEL");
    const char *tier = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_TIER") ;
    std::ostringstream oss;
    LanguageInfo info;

    oss << "/v1/listen?";

    if (!tier && !model && !customModel) {
      /* make best choice by language */
      if (getLanguageInfo(language, info)) {
        oss << "tier=" << info.tier << "&model=" << info.model;
      }
      else {
        oss << "tier=base&model=general"; // most widely supported, though not ideal
      }
    }
    else {
      if (tier) oss << "tier=" << tier;
      if (model) oss << "&model=" << model;
      if (customModel) oss << "&model=" << customModel;
    }

    if ((var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_MODEL_VERSION"))) {
     oss <<  "&version=";
     oss <<  var;
    }
    oss <<  "&language=";
    oss <<  language;

    if (channels == 2) {
     oss <<  "&multichannel=true";
     oss <<  "&channels=2";
    }

    if ((var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_ENABLE_SMART_FORMAT"))) {
     oss <<  "&smart_format=true";
     oss <<  "&no_delay=true";
     /**
      * see: https://github.com/orgs/deepgram/discussions/384
      * 
      */
    }
    if ((var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_ENABLE_AUTOMATIC_PUNCTUATION"))) {
     oss <<  "&punctuate=true";
    }
    if (switch_true(switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_PROFANITY_FILTER"))) {
     oss <<  "&profanity_filter=true";
    }
    if ((var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_REDACT"))) {
     oss <<  "&redact=";
     oss <<  var;
    }
    if (switch_true(switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_DIARIZE"))) {
     oss <<  "&diarize=true";
      if ((var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_DIARIZE_VERSION"))) {
       oss <<  "&diarize_version=";
       oss <<  var;
      }
    }
    if (switch_true(switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_NER"))) {
     oss <<  "&ner=true";
    }
    if ((var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_ALTERNATIVES"))) {
     oss <<  "&alternatives=";
     oss <<  var;
    }
    if (switch_true(switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_NUMERALS"))) {
     oss <<  "&numerals=true";
    }

		const char* hints = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_SEARCH");
		if (hints) {
			char *phrases[500] = { 0 };
      int argc = switch_separate_string((char *)hints, ',', phrases, 500);
      for (int i = 0; i < argc; i++) {
       oss <<  "&search=";
       oss <<  encodeURIComponent(phrases[i]);
      }
		}
		const char* keywords = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_KEYWORDS");
		if (keywords) {
			char *phrases[500] = { 0 };
      int argc = switch_separate_string((char *)keywords, ',', phrases, 500);
      for (int i = 0; i < argc; i++) {
       oss <<  "&keywords=";
       oss <<  encodeURIComponent(phrases[i]);
      }
		}
		const char* replace = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_REPLACE");
		if (replace) {
			char *phrases[500] = { 0 };
      int argc = switch_separate_string((char *)replace, ',', phrases, 500);
      for (int i = 0; i < argc; i++) {
       oss <<  "&replace=";
       oss <<  encodeURIComponent(phrases[i]);
      }
		}
    if ((var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_TAG"))) {
     oss <<  "&tag=";
     oss <<  var;
    }
    if (interim) {
     oss <<  "&interim_results=true";
    }
    if ((var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_ENDPOINTING"))) {
      oss <<  "&endpointing=";
      oss <<  var;
    }
    if ((var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_UTTERANCE_END_MS"))) {
      oss <<  "&utterance_end_ms=";
      oss <<  var;
    }
    if ((var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_VAD_TURNOFF"))) {
      oss <<  "&vad_turnoff=";
      oss <<  var;
    }
   oss <<  "&encoding=linear16";
   oss <<  "&sample_rate=8000";
   path = oss.str();
   return path;
  }

  static void eventCallback(const char* sessionId, deepgram::AudioPipe::NotifyEvent_t event, const char* message, bool finished) {
    SessionLock sl(sessionId);
    switch_core_session_t* session = sl.session;
    if (session) {
      switch_channel_t *channel = switch_core_session_get_channel(session);
      switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
      if (bug) {
        private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
        if (tech_pvt) {
          switch (event) {
            case deepgram::AudioPipe::CONNECT_SUCCESS:
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "connection successful\n");
              tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_CONNECT_SUCCESS, NULL, tech_pvt->bugname, finished);
            break;
            case deepgram::AudioPipe::CONNECT_FAIL:
            {
              // The AudioPipe is NOT cleared here; it stays valid until the reaper
              // deletes it during cleanup. dg_transcribe_frame gates on the
              // connection state, so it will not use a failed pipe.
              /* lws passes a NULL error string for some connect failures (the
                 AudioPipe forwards its `in` pointer as-is); streaming NULL into
                 an ostream or %s is undefined behavior */
              const char* reason = message ? message : "unknown";
              std::stringstream json;
              json << "{\"reason\":\"" << reason << "\"}";
              tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_CONNECT_FAIL, (char *) json.str().c_str(), tech_pvt->bugname, finished);
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "connection failed: %s\n", reason);
            }
            break;
            case deepgram::AudioPipe::CONNECTION_DROPPED:
              // pipe stays valid until the reaper; dg_transcribe_frame gates on state.
              tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_DISCONNECT, NULL, tech_pvt->bugname, finished);
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "connection dropped from far end\n");
            break;
            case deepgram::AudioPipe::CONNECTION_CLOSED_GRACEFULLY:
              // pipe stays valid until the reaper; dg_transcribe_frame gates on state.
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "connection closed gracefully\n");
            break;
            case deepgram::AudioPipe::MESSAGE:
              if( strstr(message, emptyTranscript)) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "discarding empty deepgram transcript\n");
              }
              else {
                tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_RESULTS, message, tech_pvt->bugname, finished);
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "deepgram message: %s\n", message);
              }
            break;

            default:
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "got unexpected msg from deepgram %d:%s\n", event, message);
              break;
          }
        }
      }
    }
  }
  switch_status_t fork_data_init(private_t *tech_pvt, switch_core_session_t *session,
    int sampling, int desiredSampling, int channels, char *lang, int interim, 
    char* bugname, responseHandler_t responseHandler) {

    int err;
    switch_codec_implementation_t read_impl;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    switch_core_session_get_read_impl(session, &read_impl);
  
    memset(tech_pvt, 0, sizeof(private_t));
  
    std::string path;
    constructPath(session, path, desiredSampling, channels, lang, interim);
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "path: %s\n", path.c_str());

    strncpy(tech_pvt->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID);
    tech_pvt->sessionId[MAX_SESSION_ID-1] = '\0';
    /* previously never populated: every event went out with an empty
       media-bugname header and the CLOSE-path stop cleared channel-private
       key "" instead of the key the bug is actually stored under */
    strncpy(tech_pvt->bugname, bugname ? bugname : MY_BUG_NAME, MAX_BUG_LEN);
    tech_pvt->bugname[MAX_BUG_LEN] = '\0';
    strncpy(tech_pvt->host, "api.deepgram.com", MAX_WS_URL_LEN);
    tech_pvt->host[MAX_WS_URL_LEN-1] = '\0';
    tech_pvt->port = 443;
    strncpy(tech_pvt->path, path.c_str(), MAX_PATH_LEN);
    tech_pvt->path[MAX_PATH_LEN-1] = '\0';
    tech_pvt->sampling = desiredSampling;
    tech_pvt->responseHandler = responseHandler;
    tech_pvt->channels = channels;
    tech_pvt->id = ++idxCallCount;
    tech_pvt->buffer_overrun_notified = 0;
    
    /* size the send buffer to hold nAudioBufferSecs of L16 audio:
       (bytes per 20ms frame) * (frames per second) * channels * seconds, plus LWS_PRE headroom */
    size_t buflen = LWS_PRE + (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * 1000 / RTP_PACKETIZATION_PERIOD * nAudioBufferSecs);

    const char* apiKey = switch_channel_get_variable(channel, "DEEPGRAM_API_KEY");
    if (!apiKey && defaultApiKey) apiKey = defaultApiKey;
    else if (!apiKey) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "no deepgram api key provided\n");
      return SWITCH_STATUS_FALSE;
    }

    deepgram::AudioPipe* ap = new deepgram::AudioPipe(tech_pvt->sessionId, tech_pvt->host, tech_pvt->port, tech_pvt->path, 
      buflen, read_impl.decoded_bytes_per_packet, apiKey, eventCallback);
    if (!ap) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error allocating AudioPipe\n");
      return SWITCH_STATUS_FALSE;
    }

    tech_pvt->pAudioPipe = static_cast<void *>(ap);

    switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));

    if (desiredSampling != sampling) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) resampling from %u to %u\n", tech_pvt->id, sampling, desiredSampling);
      tech_pvt->resampler = speex_resampler_init(channels, sampling, desiredSampling, SWITCH_RESAMPLE_QUALITY, &err);
      if (0 != err) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
        return SWITCH_STATUS_FALSE;
      }
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) no resampling needed for this call\n", tech_pvt->id);
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) fork_data_init\n", tech_pvt->id);

    return SWITCH_STATUS_SUCCESS;
  }

  void lws_logger(int level, const char *line) {
    switch_log_level_t llevel = SWITCH_LOG_DEBUG;

    switch (level) {
      case LLL_ERR: llevel = SWITCH_LOG_ERROR; break;
      case LLL_WARN: llevel = SWITCH_LOG_WARNING; break;
      case LLL_NOTICE: llevel = SWITCH_LOG_NOTICE; break;
      case LLL_INFO: llevel = SWITCH_LOG_INFO; break;
    }
	  switch_log_printf(SWITCH_CHANNEL_LOG, llevel, "%s\n", line);
  }
}


extern "C" {
  switch_status_t dg_transcribe_init() {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_deepgram_transcribe: audio buffer (in secs):    %d secs\n", nAudioBufferSecs);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_deepgram_transcribe: lws service threads:       %d\n", nServiceThreads);
 
    /* deliberate mask: preserve today's effective verbosity (ERR|WARN|NOTICE).
       Do not enable LLL_INFO/LLL_DEBUG: at scale they flood the logs. */
    int logs = LLL_ERR | LLL_WARN | LLL_NOTICE;
    
    deepgram::AudioPipe::initialize(nServiceThreads, logs, lws_logger);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "AudioPipe::initialize completed\n");

		const char* apiKey = std::getenv("DEEPGRAM_API_KEY");
		if (NULL == apiKey) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, 
				"\"DEEPGRAM_API_KEY\" env var not set; authentication will expect channel variables of same names to be set\n");
		}
		else {
			hasDefaultCredentials = true;
      defaultApiKey = apiKey;
		}
		return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t dg_transcribe_cleanup() {
    bool cleanup = false;
    cleanup = deepgram::AudioPipe::deinitialize();
    if (cleanup == true) {
        return SWITCH_STATUS_SUCCESS;
    }
    return SWITCH_STATUS_FALSE;
  }
	
  switch_status_t dg_transcribe_session_init(switch_core_session_t *session, 
    responseHandler_t responseHandler, uint32_t samples_per_second, uint32_t channels, 
    char* lang, int interim, char* bugname, void **ppUserData)
  {    	
    int err;

    // allocate per-session data structure
    private_t* tech_pvt = (private_t *) switch_core_session_alloc(session, sizeof(private_t));
    if (!tech_pvt) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "error allocating memory!\n");
      return SWITCH_STATUS_FALSE;
    }

    if (SWITCH_STATUS_SUCCESS != fork_data_init(tech_pvt, session, samples_per_second, 8000, channels, lang, interim, bugname, responseHandler)) {
      destroy_tech_pvt(tech_pvt);
      return SWITCH_STATUS_FALSE;
    }

    *ppUserData = tech_pvt;

    deepgram::AudioPipe *pAudioPipe = static_cast<deepgram::AudioPipe *>(tech_pvt->pAudioPipe);
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "connecting now\n");
    pAudioPipe->connect();
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "connection in progress\n");
    return SWITCH_STATUS_SUCCESS;
  }

  void dg_transcribe_session_cleanup(void *pUserData) {
    private_t* tech_pvt = (private_t*) pUserData;
    if (!tech_pvt) return;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
      "dg_transcribe_session_cleanup: tearing down orphaned tech_pvt (media bug never attached)\n");
    switch_mutex_lock(tech_pvt->mutex);
    /* the AudioPipe is already connecting (session_init calls connect());
       hand it to the reaper, which finishes/closes it and deletes it once the
       close promise is fulfilled -- finish() works pre-handshake via
       m_gracefulShutdown, and CONNECT_FAIL fulfills the promise itself */
    if (tech_pvt->pAudioPipe) reaper(tech_pvt);
    destroy_tech_pvt(tech_pvt);
    switch_mutex_unlock(tech_pvt->mutex);
  }

	switch_status_t dg_transcribe_session_stop(switch_core_session_t *session,int channelIsClosing, char* bugname) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "dg_transcribe_session_stop: no bug - websocket conection already closed\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
    uint32_t id = tech_pvt->id;

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) dg_transcribe_session_stop\n", id);

    if (!tech_pvt) return SWITCH_STATUS_FALSE;
      
    // close connection and get final responses
    switch_mutex_lock(tech_pvt->mutex);
    /* the bug is stored under the fixed MY_BUG_NAME key (start_capture), and
       that is also the key this function looked it up under above -- clearing
       the caller-supplied bugname instead left a stale bug pointer under
       MY_BUG_NAME for any custom name, which a later stop/start dereferenced
       after switch_core_media_bug_remove had already destroyed the bug */
    switch_channel_set_private(channel, MY_BUG_NAME, NULL);
    if (!channelIsClosing) switch_core_media_bug_remove(session, &bug);

    deepgram::AudioPipe *pAudioPipe = static_cast<deepgram::AudioPipe *>(tech_pvt->pAudioPipe);
    if (pAudioPipe) reaper(tech_pvt);
    destroy_tech_pvt(tech_pvt);
    switch_mutex_unlock(tech_pvt->mutex);
    // NB: tech_pvt->mutex was created from the session pool
    // (switch_core_session_get_pool); it is owned by the pool and freed when the
    // pool is destroyed. Do NOT switch_mutex_destroy() it here — that would be a
    // double cleanup.
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) dg_transcribe_session_stop\n", id);
    return SWITCH_STATUS_SUCCESS;
  }
	
	switch_bool_t dg_transcribe_frame(switch_core_session_t *session, switch_media_bug_t *bug) {
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
    size_t inuse = 0;
    bool dirty = false;
    char *p = (char *) "{\"msg\": \"buffer overrun\"}";

    if (!tech_pvt) return SWITCH_TRUE;
    
    if (switch_mutex_trylock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {
      /* lock already acquired via trylock above; guard unlocks it on every exit path */
      MutexUnlockGuard mlk(tech_pvt->mutex);
      if (!tech_pvt->pAudioPipe) {
        return SWITCH_TRUE;
      }
      deepgram::AudioPipe *pAudioPipe = static_cast<deepgram::AudioPipe *>(tech_pvt->pAudioPipe);
      if (pAudioPipe->getLwsState() != deepgram::AudioPipe::LWS_CLIENT_CONNECTED) {
        return SWITCH_TRUE;
      }

      pAudioPipe->lockAudioBuffer();
      size_t available = pAudioPipe->binarySpaceAvailable();
      if (NULL == tech_pvt->resampler) {
        switch_frame_t frame = { 0 };
        frame.data = pAudioPipe->binaryWritePtr();
        frame.buflen = available;
        while (true) {

          // check if buffer would be overwritten; dump packets if so
          if (available < pAudioPipe->binaryMinSpace()) {
            if (!tech_pvt->buffer_overrun_notified) {
              tech_pvt->buffer_overrun_notified = 1;
              tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_BUFFER_OVERRUN, NULL, tech_pvt->bugname, 0);
            }
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%u) dropping packets!\n", 
              tech_pvt->id);
            pAudioPipe->binaryWritePtrResetToZero();

            frame.data = pAudioPipe->binaryWritePtr();
            frame.buflen = available = pAudioPipe->binarySpaceAvailable();
          }

          switch_status_t rv = switch_core_media_bug_read(bug, &frame, SWITCH_TRUE);
          if (rv != SWITCH_STATUS_SUCCESS) break;
          if (frame.datalen) {
            pAudioPipe->binaryWritePtrAdd(frame.datalen);
            frame.buflen = available = pAudioPipe->binarySpaceAvailable();
            frame.data = pAudioPipe->binaryWritePtr();
            dirty = true;
          }
        }
      }
      else {
        uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
        switch_frame_t frame = { 0 };
        frame.data = data;
        frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;
        while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
          if (frame.datalen) {
            /* speex's interleaved API takes out_len in samples PER CHANNEL and
               may write out_len * channels samples (2 bytes each). Sizing it as
               available/2 ignored the channel count and let a stereo resample
               write up to 2x the remaining ring-buffer space (heap overflow
               past m_audio_buffer). Divide by bytes-per-frame instead; the
               bytes_written accounting below already multiplies back. */
            spx_uint32_t out_len = available / (2 * tech_pvt->channels);
            spx_uint32_t in_len = frame.samples;

            speex_resampler_process_interleaved_int(tech_pvt->resampler, 
              (const spx_int16_t *) frame.data, 
              (spx_uint32_t *) &in_len, 
              (spx_int16_t *) ((char *) pAudioPipe->binaryWritePtr()),
              &out_len);

            if (out_len > 0) {
              // bytes written = num samples * 2 * num channels
              size_t bytes_written = out_len * 2 * tech_pvt->channels;
              pAudioPipe->binaryWritePtrAdd(bytes_written);
              available = pAudioPipe->binarySpaceAvailable();
              dirty = true;
            }
            if (available < pAudioPipe->binaryMinSpace()) {
              if (!tech_pvt->buffer_overrun_notified) {
                tech_pvt->buffer_overrun_notified = 1;
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%u) dropping packets!\n", 
                  tech_pvt->id);
                tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_BUFFER_OVERRUN, NULL, tech_pvt->bugname, 0);
              }
              break;
            }
          }
        }
      }

      pAudioPipe->unlockAudioBuffer();
      /* mlk unlocks tech_pvt->mutex here as it goes out of scope */
    }
    return SWITCH_TRUE;
  }
}
