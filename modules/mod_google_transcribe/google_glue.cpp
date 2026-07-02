#include <cstdlib>
#include <algorithm>
#include <future>
#include <memory>
#include <mutex>

#include <switch.h>
#include <switch_json.h>
#include <grpc++/grpc++.h>

#include "google/cloud/speech/v1p1beta1/cloud_speech.grpc.pb.h"

#include <switch_json.h>

#include "mod_google_transcribe.h"
#include "simple_buffer.h"

using google::cloud::speech::v1p1beta1::RecognitionConfig;
using google::cloud::speech::v1p1beta1::Speech;
using google::cloud::speech::v1p1beta1::SpeechContext;
using google::cloud::speech::v1p1beta1::StreamingRecognizeRequest;
using google::cloud::speech::v1p1beta1::StreamingRecognizeResponse;
using google::cloud::speech::v1p1beta1::SpeakerDiarizationConfig;
using google::cloud::speech::v1p1beta1::SpeechAdaptation;
using google::cloud::speech::v1p1beta1::PhraseSet;
using google::cloud::speech::v1p1beta1::PhraseSet_Phrase;
using google::cloud::speech::v1p1beta1::RecognitionMetadata;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_InteractionType_DISCUSSION;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_InteractionType_PRESENTATION;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_InteractionType_PHONE_CALL;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_InteractionType_VOICEMAIL;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_InteractionType_PROFESSIONALLY_PRODUCED;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_InteractionType_VOICE_SEARCH;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_InteractionType_VOICE_COMMAND;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_InteractionType_DICTATION;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_MicrophoneDistance_NEARFIELD;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_MicrophoneDistance_MIDFIELD;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_MicrophoneDistance_FARFIELD;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_OriginalMediaType_AUDIO;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_OriginalMediaType_VIDEO;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_RecordingDeviceType_SMARTPHONE;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_RecordingDeviceType_PC;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_RecordingDeviceType_PHONE_LINE;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_RecordingDeviceType_VEHICLE;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_RecordingDeviceType_OTHER_OUTDOOR_DEVICE;
using google::cloud::speech::v1p1beta1::RecognitionMetadata_RecordingDeviceType_OTHER_INDOOR_DEVICE;
using google::cloud::speech::v1p1beta1::StreamingRecognizeResponse_SpeechEventType_END_OF_SINGLE_UTTERANCE;
using google::rpc::Status;

#define CHUNKSIZE (320)  /* bytes per audio chunk: 320 = 160 samples = 20ms @ 8kHz, 16-bit mono (L16) */

/* number of CHUNKSIZE chunks to pre-buffer while waiting for the gRPC stream to connect (VAD path) */
#define PREBUFFER_CHUNKS (15)

namespace {
  int case_insensitive_match(std::string s1, std::string s2) {
   std::transform(s1.begin(), s1.end(), s1.begin(), ::tolower);
   std::transform(s2.begin(), s2.end(), s2.begin(), ::tolower);
   if(s1.compare(s2) == 0)
      return 1; //The strings are same
   return 0; //not matched
  }

  // RAII guard for switch_core_session_locate / switch_core_session_rwunlock.
  // Locates the session on construction; releases the read-lock on destruction
  // (any scope exit). get() returns nullptr if the session could not be located.
  class SessionLock {
  public:
    explicit SessionLock(const char* sessionId) : m_session(switch_core_session_locate(sessionId)) {}
    ~SessionLock() { if (m_session) switch_core_session_rwunlock(m_session); }
    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;
    switch_core_session_t* get() const { return m_session; }
  private:
    switch_core_session_t* m_session;
  };

  // RAII guard for switch_mutex_lock / switch_mutex_unlock around scopes with
  // multiple exits. Locks on construction, unlocks on destruction.
  class MutexLock {
  public:
    explicit MutexLock(switch_mutex_t* mutex) : m_mutex(mutex) { switch_mutex_lock(m_mutex); }
    ~MutexLock() { switch_mutex_unlock(m_mutex); }
    MutexLock(const MutexLock&) = delete;
    MutexLock& operator=(const MutexLock&) = delete;
  private:
    switch_mutex_t* m_mutex;
  };
}
class GStreamer;

class GStreamer {
public:
	GStreamer(
    switch_core_session_t *session, 
    uint32_t channels, 
    char* lang, 
    int interim, 
    uint32_t config_sample_rate,
		uint32_t samples_per_second,
    int single_utterance, 
    int separate_recognition,
		int max_alternatives, 
    int profanity_filter, 
    int word_time_offset, 
    int punctuation, 
    const char* model, 
    int enhanced, 
		const char* hints) : m_session(session), m_writesDone(false), m_connected(false) {
  
    const char* var;
    const char* google_uri;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    if (!(google_uri = switch_channel_get_variable(channel, "GOOGLE_SPEECH_TO_TEXT_URI"))) {
      google_uri = "speech.googleapis.com";
    }
		/* HTTP/2 keepalive: without it a silent network partition leaves the
		   read thread blocked in Read() and cleanup's WritesDone() blocked in
		   Pluck() with no deadline -- stop/hangup then stalls for the TCP
		   failure-detection duration (minutes). Pings only while the call is
		   active; a healthy long-running stream is unaffected. */
		grpc::ChannelArguments channelArgs;
		channelArgs.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 60000);
		channelArgs.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 20000);
		channelArgs.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 0);

		if ((var = switch_channel_get_variable(channel, "GOOGLE_APPLICATION_CREDENTIALS"))) {
			auto channelCreds = grpc::SslCredentials(grpc::SslCredentialsOptions());
			auto callCreds = grpc::ServiceAccountJWTAccessCredentials(var);
			if (!callCreds) {
				/* an invalid JSON key (e.g. the channel var set to a file PATH,
				   matching the env var convention of the same name) yields a null
				   shared_ptr, and CompositeChannelCredentials dereferences it with
				   no guard (grpc v1.60 secure_credentials.cc) -- a segfault the
				   session_init catch cannot stop. Fail catchably instead. */
				throw std::invalid_argument("GOOGLE_APPLICATION_CREDENTIALS channel variable is not a valid service-account JSON key (note: unlike the env var, it must contain the key itself, not a file path)");
			}
			auto creds = grpc::CompositeChannelCredentials(channelCreds, callCreds);
			m_channel = grpc::CreateCustomChannel(google_uri, creds, channelArgs);
		}
		else {
			auto creds = grpc::GoogleDefaultCredentials();
			m_channel = grpc::CreateCustomChannel(google_uri, creds, channelArgs);
		}

  	m_stub = Speech::NewStub(m_channel);
  		
		auto* streaming_config = m_request.mutable_streaming_config();
		RecognitionConfig* config = streaming_config->mutable_config();

    streaming_config->set_interim_results(interim);
    if (single_utterance == 1) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "enable_single_utterance\n");
      streaming_config->set_single_utterance(true);
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "enable_single_utterance is FALSE\n");
      streaming_config->set_single_utterance(false);
    }

		config->set_language_code(lang);
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "transcribe language %s \n", lang);
    
  	config->set_sample_rate_hertz(config_sample_rate);

		config->set_encoding(RecognitionConfig::LINEAR16);

    // the rest of config comes from channel vars

    // number of channels in the audio stream (default: 1)
    if (channels > 1) {
      config->set_audio_channel_count(channels);
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "audio_channel_count %d\n", channels);

      // transcribe each separately?
      if (separate_recognition == 1) {
        config->set_enable_separate_recognition_per_channel(true);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "enable_separate_recognition_per_channel on\n");
      }
    }

    // max alternatives
    if (max_alternatives > 1) {
      config->set_max_alternatives(max_alternatives);
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "max_alternatives %d\n", max_alternatives);
    }

    // profanity filter
    if (profanity_filter == 1) {
      config->set_profanity_filter(true);
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "profanity_filter\n");
    }

    // enable word offsets
    if (word_time_offset == 1) {
      config->set_enable_word_time_offsets(true);
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "enable_word_time_offsets\n");
    }

    // enable automatic punctuation
    if (punctuation == 1) {
      config->set_enable_automatic_punctuation(true);
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "enable_automatic_punctuation\n");
    }
    else {
      config->set_enable_automatic_punctuation(false);
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "disable_automatic_punctuation\n");
    }

    // speech model
    if (model != NULL) {
      config->set_model(model);
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "speech model %s\n", model);
    }

    // use enhanced model
    if (enhanced == 1) {
      config->set_use_enhanced(true);
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "use_enhanced\n");
    }

    // hints  
    if (hints != NULL) {
      auto* adaptation = config->mutable_adaptation();
      auto* phrase_set = adaptation->add_phrase_sets();
      auto *context = config->add_speech_contexts();
      float boost = -1;

      // get boost setting for the phrase set in its entirety. Gate on the var
      // being SET, not switch_true(): fractional boosts ("0.5") atoi to 0 and
      // were silently ignored, while "true"-ish garbage passed and set 0.0.
      if ((var = switch_channel_get_variable(channel, "GOOGLE_SPEECH_HINTS_BOOST"))) {
     	  boost = (float) atof(var);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "boost value: %f\n", boost);
        phrase_set->set_boost(boost);
      }

      // hints are either a simple comma-separated list of phrases, or a json array of objects
      // containing a phrase and a boost value
      auto *jHint = cJSON_Parse((char *) hints);
      if (jHint) {
        int i = 0;
        cJSON *jPhrase = NULL;
        cJSON_ArrayForEach(jPhrase, jHint) {
          cJSON *jItem = cJSON_GetObjectItem(jPhrase, "phrase");
          /* cJSON_GetStringValue returns NULL for a non-string "phrase"
             ([{"phrase":123}]) and protobuf set_value(const char*) would
             construct std::string from NULL -- crash on operator-provided
             config. Also stop add_phrases()ing empty entries for objects
             with no usable phrase. */
          const char* sPhrase = jItem ? cJSON_GetStringValue(jItem) : NULL;
          if (sPhrase) {
            auto* phrase = phrase_set->add_phrases();
            phrase->set_value(sPhrase);
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "phrase: %s\n", phrase->value().c_str());
            if (cJSON_GetObjectItem(jPhrase, "boost")) {
              phrase->set_boost((float) cJSON_GetObjectItem(jPhrase, "boost")->valuedouble);
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "boost value: %f\n", phrase->boost());
            }
            i++;
          }
        }
        cJSON_Delete(jHint);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "added %d hints\n", i);
      }
      else {
        char *phrases[500] = { 0 };
        int argc = switch_separate_string((char *) hints, ',', phrases, 500);
        for (int i = 0; i < argc; i++) {
          auto* phrase = phrase_set->add_phrases();
          phrase->set_value(phrases[i]);
        }
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "added %d hints\n", argc);
      }
    }

    // alternative language
    if ((var = switch_channel_get_variable(channel, "GOOGLE_SPEECH_ALTERNATIVE_LANGUAGE_CODES"))) {
      char *alt_langs[3] = { 0 };
      int argc = switch_separate_string((char *) var, ',', alt_langs, 3);
      for (int i = 0; i < argc; i++) {
        config->add_alternative_language_codes(alt_langs[i]);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "added alternative lang %s\n", alt_langs[i]);
      }
    }

    // speaker diarization
    if ((var = switch_channel_get_variable(channel, "GOOGLE_SPEECH_SPEAKER_DIARIZATION"))) {
      auto* diarization_config = config->mutable_diarization_config();
      diarization_config->set_enable_speaker_diarization(true);
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "enabling speaker diarization\n", var);
      if ((var = switch_channel_get_variable(channel, "GOOGLE_SPEECH_SPEAKER_DIARIZATION_MIN_SPEAKER_COUNT"))) {
        int count = std::max(atoi(var), 1);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "setting min speaker count to %d\n", count);
        diarization_config->set_min_speaker_count(count);
      }
      if ((var = switch_channel_get_variable(channel, "GOOGLE_SPEECH_SPEAKER_DIARIZATION_MAX_SPEAKER_COUNT"))) {
        int count = std::max(atoi(var), 2);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "setting max speaker count to %d\n", count);
        diarization_config->set_max_speaker_count(count);
      }
    }

    // recognition metadata
    auto* metadata = config->mutable_metadata();
    if ((var = switch_channel_get_variable(channel, "GOOGLE_SPEECH_METADATA_INTERACTION_TYPE"))) {
      if (case_insensitive_match("discussion", var)) metadata->set_interaction_type(RecognitionMetadata_InteractionType_DISCUSSION);
      if (case_insensitive_match("presentation", var)) metadata->set_interaction_type(RecognitionMetadata_InteractionType_PRESENTATION);
      if (case_insensitive_match("phone_call", var)) metadata->set_interaction_type(RecognitionMetadata_InteractionType_PHONE_CALL);
      if (case_insensitive_match("voicemail", var)) metadata->set_interaction_type(RecognitionMetadata_InteractionType_VOICEMAIL);
      if (case_insensitive_match("professionally_produced", var)) metadata->set_interaction_type(RecognitionMetadata_InteractionType_PROFESSIONALLY_PRODUCED);
      if (case_insensitive_match("voice_search", var)) metadata->set_interaction_type(RecognitionMetadata_InteractionType_VOICE_SEARCH);
      if (case_insensitive_match("voice_command", var)) metadata->set_interaction_type(RecognitionMetadata_InteractionType_VOICE_COMMAND);
      if (case_insensitive_match("dictation", var)) metadata->set_interaction_type(RecognitionMetadata_InteractionType_DICTATION);
    }
    if ((var = switch_channel_get_variable(channel, "GOOGLE_SPEECH_METADATA_INDUSTRY_NAICS_CODE"))) {
      metadata->set_industry_naics_code_of_audio(atoi(var));
    }
    if ((var = switch_channel_get_variable(channel, "GOOGLE_SPEECH_METADATA_MICROPHONE_DISTANCE"))) {
      if (case_insensitive_match("nearfield", var)) metadata->set_microphone_distance(RecognitionMetadata_MicrophoneDistance_NEARFIELD);
      if (case_insensitive_match("midfield", var)) metadata->set_microphone_distance(RecognitionMetadata_MicrophoneDistance_MIDFIELD);
      if (case_insensitive_match("farfield", var)) metadata->set_microphone_distance(RecognitionMetadata_MicrophoneDistance_FARFIELD);
    }
    if ((var = switch_channel_get_variable(channel, "GOOGLE_SPEECH_METADATA_ORIGINAL_MEDIA_TYPE"))) {
      if (case_insensitive_match("audio", var)) metadata->set_original_media_type(RecognitionMetadata_OriginalMediaType_AUDIO);
      if (case_insensitive_match("video", var)) metadata->set_original_media_type(RecognitionMetadata_OriginalMediaType_VIDEO);
    }
    if ((var = switch_channel_get_variable(channel, "GOOGLE_SPEECH_METADATA_RECORDING_DEVICE_TYPE"))) {
      if (case_insensitive_match("smartphone", var)) metadata->set_recording_device_type(RecognitionMetadata_RecordingDeviceType_SMARTPHONE);
      if (case_insensitive_match("pc", var)) metadata->set_recording_device_type(RecognitionMetadata_RecordingDeviceType_PC);
      if (case_insensitive_match("phone_line", var)) metadata->set_recording_device_type(RecognitionMetadata_RecordingDeviceType_PHONE_LINE);
      if (case_insensitive_match("vehicle", var)) metadata->set_recording_device_type(RecognitionMetadata_RecordingDeviceType_VEHICLE);
      if (case_insensitive_match("other_outdoor_device", var)) metadata->set_recording_device_type(RecognitionMetadata_RecordingDeviceType_OTHER_OUTDOOR_DEVICE);
      if (case_insensitive_match("other_indoor_device", var)) metadata->set_recording_device_type(RecognitionMetadata_RecordingDeviceType_OTHER_INDOOR_DEVICE);
    }
	}

	~GStreamer() {
		//switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_INFO, "GStreamer::~GStreamer - deleting channel and stub: %p\n", (void*)this);
	}

  void connect() {
    assert(!m_connected);
    // Begin a stream.
  	m_streamer = m_stub->StreamingRecognize(&m_context);
    m_connected = true;

    // read thread is waiting on this
    m_promise.set_value();

  	// Write the first request, containing the config only.
  	// gRPC requires all write-side ops (Write + WritesDone) on a stream to be
  	// externally serialized; m_write_mutex serializes them across threads.
  	{
  	  std::lock_guard<std::mutex> lk(m_write_mutex);
  	  m_streamer->Write(m_request);
  	}

    // send any buffered audio (only the VAD path ever fills m_audioBuffer)
    int nFrames = m_audioBuffer ? m_audioBuffer->getNumItems() : 0;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer %p got stream ready, %d buffered frames\n", this, nFrames);
    if (nFrames) {
      char *p;
      do {
        p = m_audioBuffer->getNextChunk();
        if (p) {
          write(p, CHUNKSIZE);
        }
      } while (p);
    }
  }

	bool write(void* data, uint32_t datalen) {
    if (!m_connected) {
      if (datalen % CHUNKSIZE == 0) {
        // lazily allocate the prebuffer: only the VAD path writes before connect()
        if (!m_audioBuffer) {
          m_audioBuffer.reset(new SimpleBuffer(CHUNKSIZE, PREBUFFER_CHUNKS));
        }
        m_audioBuffer->add(data, datalen);
      }
      return true;
    }
    m_request.set_audio_content(data, datalen);
    std::lock_guard<std::mutex> lk(m_write_mutex);
    /* re-check under the lock: after WritesDone()/Finish() the stream is
       half-closed and a further Write violates the gRPC API contract (the
       END_OF_SINGLE_UTTERANCE gate in the frame callback is only checked once,
       before its read loop, so the read thread can half-close mid-loop) */
    if (m_writesDone) return false;
    bool ok = m_streamer->Write(m_request);
    return ok;
  }

	uint32_t nextMessageSize(void) {
		uint32_t size = 0;
		m_streamer->NextMessageSize(&size);
		return size;
	}

	bool read(StreamingRecognizeResponse* response) {
		return m_streamer->Read(response);
	}

	grpc::Status finish() {
		/* gRPC sync streams document Write/WritesDone as thread-safe only with
		   respect to Read -- NOT Finish. The media thread can still be inside
		   write() when google unilaterally ends the stream and the read thread
		   calls finish(); serialize them on the same mutex, and mark the write
		   side closed so no Write can follow Finish. */
		std::lock_guard<std::mutex> lk(m_write_mutex);
		m_writesDone = true;
		return m_streamer->Finish();
	}

	void writesDone() {
    // grpc crashes if we call this twice on a stream
    if (!m_connected) {
      cancelConnect();
    }
    else {
      /* the flag must be re-checked UNDER the lock: the gRPC read thread
         (END_OF_SINGLE_UTTERANCE) and the stop/hangup cleanup thread can both
         reach here concurrently -- with the check outside the lock both passed
         it, then serialized on the mutex and each issued WritesDone(), the
         double half-close this function's own comment says crashes grpc */
      std::lock_guard<std::mutex> lk(m_write_mutex);
      if (!m_writesDone) {
        m_streamer->WritesDone();
        m_writesDone = true;
      }
    }
	}

  bool waitForConnect() {
    std::shared_future<void> sf(m_promise.get_future());
    sf.wait();
    return m_connected;
  }

  void cancelConnect() {
    assert(!m_connected);
    m_promise.set_value();
  }

  bool isConnected() {
    return m_connected;
  }

private:
	switch_core_session_t* m_session;
  grpc::ClientContext m_context;
	std::shared_ptr<grpc::Channel> m_channel;
	std::unique_ptr<Speech::Stub> 	m_stub;
	std::unique_ptr< grpc::ClientReaderWriterInterface<StreamingRecognizeRequest, StreamingRecognizeResponse> > m_streamer;
	StreamingRecognizeRequest m_request;
  // m_writesDone and m_connected are read/written across the media (frame)
  // thread, the gRPC read thread, and the cleanup thread without a shared lock
  // (e.g. writesDone() is called from both the gRPC read thread and cleanup; a
  // VAD-path connect() on the media thread can run concurrently with cleanup's
  // writesDone()). They are atomics to make those accesses race-free; a mutex
  // is avoided because cleanup joins the read thread while it may be writing.
  std::atomic<bool> m_writesDone;
  std::atomic<bool> m_connected;
  std::mutex m_write_mutex;  // serializes all m_streamer write-side ops (Write/WritesDone)
  std::promise<void> m_promise;
  std::unique_ptr<SimpleBuffer> m_audioBuffer;  // lazily allocated; only used on the VAD prebuffer path
};

static void *SWITCH_THREAD_FUNC grpc_read_thread(switch_thread_t *thread, void *obj) {
	struct cap_cb *cb = (struct cap_cb *) obj;
	GStreamer* streamer = (GStreamer *) cb->streamer.load();

  bool connected = streamer->waitForConnect();
  if (!connected) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "google transcribe grpc read thread exiting since we didnt connect\n") ;
    return nullptr;
  }

  // Read responses.
  StreamingRecognizeResponse response;
  while (streamer->read(&response)) {  // Returns false when no more to read.
    SessionLock sessionLock(cb->sessionId);
    switch_core_session_t* session = sessionLock.get();
    if (!session) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "grpc_read_thread: session %s is gone!\n", cb->sessionId) ;
      return nullptr;
    }
    auto speech_event_type = response.speech_event_type();
    if (response.has_error()) {
      Status status = response.error();
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "grpc_read_thread: error %s (%d)\n", status.message().c_str(), status.code()) ;
      cJSON* json = cJSON_CreateObject();
      cJSON_AddStringToObject(json, "type", "error");
      cJSON_AddStringToObject(json, "error", status.message().c_str());
      char* jsonString = cJSON_PrintUnformatted(json);
      cb->responseHandler(session, jsonString, cb->bugname);
      free(jsonString);
      cJSON_Delete(json);
    }
    
    if (cb->play_file == 1){
      cb->responseHandler(session, "play_interrupt", cb->bugname);
    }
    
    for (int r = 0; r < response.results_size(); ++r) {
      const auto& result = response.results(r);
      cJSON * jResult = cJSON_CreateObject();
      cJSON * jAlternatives = cJSON_CreateArray();
      cJSON * jStability = cJSON_CreateNumber(result.stability());
      cJSON * jIsFinal = cJSON_CreateBool(result.is_final());
      cJSON * jLanguageCode = cJSON_CreateString(result.language_code().c_str());
      cJSON * jChannelTag = cJSON_CreateNumber(result.channel_tag());

      auto duration = result.result_end_time();
      int32_t seconds = duration.seconds();
      int64_t nanos = duration.nanos();
      int span = (int) trunc(seconds * 1000. + ((float) nanos / 1000000.));
      cJSON * jResultEndTime = cJSON_CreateNumber(span);

      cJSON_AddItemToObject(jResult, "stability", jStability);
      cJSON_AddItemToObject(jResult, "is_final", jIsFinal);
      cJSON_AddItemToObject(jResult, "alternatives", jAlternatives);
      cJSON_AddItemToObject(jResult, "language_code", jLanguageCode);
      cJSON_AddItemToObject(jResult, "channel_tag", jChannelTag);
      cJSON_AddItemToObject(jResult, "result_end_time", jResultEndTime);

      for (int a = 0; a < result.alternatives_size(); ++a) {
        const auto& alternative = result.alternatives(a);
        cJSON* jAlt = cJSON_CreateObject();
        cJSON* jConfidence = cJSON_CreateNumber(alternative.confidence());
        cJSON* jTranscript = cJSON_CreateString(alternative.transcript().c_str());
        cJSON_AddItemToObject(jAlt, "confidence", jConfidence);
        cJSON_AddItemToObject(jAlt, "transcript", jTranscript);

        if (alternative.words_size() > 0) {
          cJSON * jWords = cJSON_CreateArray();
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "grpc_read_thread: %d words\n", alternative.words_size()) ;
          for (int b = 0; b < alternative.words_size(); b++) {
            const auto& words = alternative.words(b);
            cJSON* jWord = cJSON_CreateObject();
            cJSON_AddItemToObject(jWord, "word", cJSON_CreateString(words.word().c_str()));
            if (words.has_start_time()) {
              cJSON_AddItemToObject(jWord, "start_time", cJSON_CreateNumber(words.start_time().seconds()));
            }
            if (words.has_end_time()) {
              cJSON_AddItemToObject(jWord, "end_time", cJSON_CreateNumber(words.end_time().seconds()));
            }
            int speaker_tag = words.speaker_tag();
            if (speaker_tag > 0) {
              cJSON_AddItemToObject(jWord, "speaker_tag", cJSON_CreateNumber(speaker_tag));
            }
            float confidence = words.confidence();
            if (confidence > 0.0) {
              cJSON_AddItemToObject(jWord, "confidence", cJSON_CreateNumber(confidence));
            }

            cJSON_AddItemToArray(jWords, jWord);
          }
          cJSON_AddItemToObject(jAlt, "words", jWords);
        }
        cJSON_AddItemToArray(jAlternatives, jAlt);
      }

      char* json = cJSON_PrintUnformatted(jResult);
      cb->responseHandler(session, (const char *) json, cb->bugname);
      free(json);

      cJSON_Delete(jResult);
    }

    if (speech_event_type == StreamingRecognizeResponse_SpeechEventType_END_OF_SINGLE_UTTERANCE) {
      // we only get this when we have requested it, and recognition stops after we get this
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "grpc_read_thread: got end_of_utterance\n") ;
      cb->got_end_of_utterance = 1;
      cb->responseHandler(session, "end_of_utterance", cb->bugname);
      if (cb->wants_single_utterance) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "grpc_read_thread: sending writesDone because we want only a single utterance\n") ;
        streamer->writesDone();
      }
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "grpc_read_thread: got %d responses\n", response.results_size());
    // sessionLock releases the session read-lock here at scope exit
  }

  {
    SessionLock sessionLock(cb->sessionId);
    switch_core_session_t* session = sessionLock.get();
    if (session) {
      grpc::Status status = streamer->finish();
      if (11 == status.error_code()) {
        if (std::string::npos != status.error_message().find("Exceeded maximum allowed stream duration")) {
          cb->responseHandler(session, "max_duration_exceeded", cb->bugname);
        }
        else {
          cb->responseHandler(session, "no_audio", cb->bugname);
        }
      }
      else if (!status.ok()) {
        /* the RPC failure status (UNAUTHENTICATED, UNAVAILABLE,
           INVALID_ARGUMENT, RESOURCE_EXHAUSTED, ...) arrives HERE, not as an
           in-band response error -- previously only OUT_OF_RANGE was surfaced
           and every other failure produced a DEBUG log and silence, leaving
           the consumer waiting on a recognizer that was gone. Same event
           shape as the in-band error path above. */
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "grpc_read_thread: stream failed: %s (%d)\n", status.error_message().c_str(), status.error_code());
        cJSON* json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "type", "error");
        cJSON_AddStringToObject(json, "error", status.error_message().c_str());
        char* jsonString = cJSON_PrintUnformatted(json);
        if (jsonString) {
          cb->responseHandler(session, jsonString, cb->bugname);
          free(jsonString);
        }
        cJSON_Delete(json);
      }
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "grpc_read_thread: finish() status %s (%d)\n", status.error_message().c_str(), status.error_code()) ;
    }
    // sessionLock releases the session read-lock here at scope exit
  }
  return nullptr;
}

/* Shared teardown: half-close (or cancel) the stream, join the read thread,
   delete the streamer, free resampler/vad. Caller must hold cb->mutex (or own
   the cb exclusively, as on the never-attached path). Idempotent: safe when
   another path already ran it. */
static void reap_streamer(struct cap_cb* cb) {
  GStreamer* streamer = (GStreamer *) cb->streamer.load();
  if (streamer) {
    streamer->writesDone();
  }
  if (cb->thread) {
    switch_status_t st;
    switch_thread_join(&st, cb->thread);
    cb->thread = NULL;
  }
  if (streamer) {
    delete streamer;
    cb->streamer.store(NULL);
  }
  if (cb->resampler) {
    speex_resampler_destroy(cb->resampler);
    cb->resampler = NULL;
  }
  if (cb->vad) {
    switch_vad_destroy(&cb->vad);
    cb->vad = nullptr;
  }
}

extern "C" {

    switch_status_t google_speech_init() {
      const char* gcsServiceKeyFile = std::getenv("GOOGLE_APPLICATION_CREDENTIALS");
      if (gcsServiceKeyFile) {
        try {
          auto creds = grpc::GoogleDefaultCredentials();
        } catch (const std::exception& e) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, 
            "Error initializing google api with provided credentials in %s: %s\n", gcsServiceKeyFile, e.what());
          return SWITCH_STATUS_FALSE;
        }
      }
      return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t google_speech_cleanup() {
      return SWITCH_STATUS_SUCCESS;
    }
    switch_status_t google_speech_session_init(switch_core_session_t *session, responseHandler_t responseHandler, 
          uint32_t to_rate, uint32_t samples_per_second, uint32_t channels, char* lang, int interim, char *bugname,
          int single_utterance, int separate_recognition, int max_alternatives, int profanity_filter, int word_time_offset,
          int punctuation, const char* model, int enhanced, const char* hints, char* play_file, void **ppUserData) {

      switch_channel_t *channel = switch_core_session_get_channel(session);
      auto read_codec = switch_core_session_get_read_codec(session);
      if (!read_codec || !read_codec->implementation) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
          "google_speech_session_init: no read codec/implementation available\n");
        return SWITCH_STATUS_FALSE;
      }
      uint32_t sampleRate = read_codec->implementation->actual_samples_per_second;
      struct cap_cb *cb;
      int err;

      cb =(struct cap_cb *) switch_core_session_alloc(session, sizeof(*cb));
      strncpy(cb->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID);
      cb->sessionId[MAX_SESSION_ID] = '\0';
      strncpy(cb->bugname, bugname, MAX_BUG_LEN);
      cb->bugname[MAX_BUG_LEN] = '\0';
      cb->got_end_of_utterance = 0;
      cb->wants_single_utterance = single_utterance;
      cb->channels = channels;
      if (play_file != NULL){
        cb->play_file = 1;
      }
      
      switch_mutex_init(&cb->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));
      if (sampleRate != to_rate) {
          cb->resampler = speex_resampler_init(channels, sampleRate, to_rate, SWITCH_RESAMPLE_QUALITY, &err);
        if (0 != err) {
           switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s: Error initializing resampler: %s.\n",
                                 switch_channel_get_name(channel), speex_resampler_strerror(err));
          if (cb->resampler) {
            speex_resampler_destroy(cb->resampler);
            cb->resampler = NULL;
          }
          return SWITCH_STATUS_FALSE;
        }
      } else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s: no resampling needed for this call\n", switch_channel_get_name(channel));
      }
      cb->responseHandler = responseHandler;

      // allocate vad if we are delaying connecting to the recognizer until we detect speech
      if (switch_channel_var_true(channel, "START_RECOGNIZING_ON_VAD")) {
        cb->vad = switch_vad_init(sampleRate, channels);
        if (cb->vad) {
          const char* var;
          int mode = 2;
          int silence_ms = 150;
          int voice_ms = 250;
          int debug = 0;

          if ((var = switch_channel_get_variable(channel, "RECOGNIZER_VAD_MODE"))) {
            mode = atoi(var);
          }
          if ((var = switch_channel_get_variable(channel, "RECOGNIZER_VAD_SILENCE_MS"))) {
            silence_ms = atoi(var);
          }
          if ((var = switch_channel_get_variable(channel, "RECOGNIZER_VAD_VOICE_MS"))) {
            voice_ms = atoi(var);
          }
          if ((var = switch_channel_get_variable(channel, "RECOGNIZER_VAD_VOICE_MS"))) {
            voice_ms = atoi(var);
          }
          switch_vad_set_mode(cb->vad, mode);
          switch_vad_set_param(cb->vad, "silence_ms", silence_ms);
          switch_vad_set_param(cb->vad, "voice_ms", voice_ms);
          switch_vad_set_param(cb->vad, "debug", debug);
        }
      }

      GStreamer *streamer = NULL;
      try {
        streamer = new GStreamer(session, channels, lang, interim, to_rate, sampleRate, single_utterance, separate_recognition, max_alternatives,
         profanity_filter, word_time_offset, punctuation, model, enhanced, hints);
        cb->streamer.store(streamer);
      } catch (std::exception& e) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s: Error initializing gstreamer: %s.\n", 
          switch_channel_get_name(channel), e.what());
        return SWITCH_STATUS_FALSE;
      }

      if (!cb->vad) streamer->connect();

      // create the read thread
      switch_threadattr_t *thd_attr = NULL;
      switch_memory_pool_t *pool = switch_core_session_get_pool(session);

      switch_threadattr_create(&thd_attr, pool);
      switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
      switch_thread_create(&cb->thread, thd_attr, grpc_read_thread, cb, pool);

      *ppUserData = cb;
      return SWITCH_STATUS_SUCCESS;
    }

    /* teardown for a cap_cb whose media bug was never attached (bug-add
       failure): the read thread is already running -- blocked in
       waitForConnect() on the VAD path or reading on the connected path --
       and it lives in the session pool, so leaving it running is a
       use-after-free once the session is destroyed */
    void google_speech_session_orphan_cleanup(void *pUserData) {
      struct cap_cb *cb = (struct cap_cb *) pUserData;
      if (!cb) return;
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
        "google_speech_session_orphan_cleanup: tearing down orphaned cb %p (media bug never attached)\n", (void *) cb);
      MutexLock cbLock(cb->mutex);
      reap_streamer(cb);
    }

    switch_status_t google_speech_session_cleanup(switch_core_session_t *session, int channelIsClosing, switch_media_bug_t *bug) {
      switch_channel_t *channel = switch_core_session_get_channel(session);

      if (bug) {
        struct cap_cb *cb = (struct cap_cb *) switch_core_media_bug_get_user_data(bug);
        MutexLock cbLock(cb->mutex);  // unlocks on any exit from this scope

        if (!switch_channel_get_private(channel, cb->bugname)) {
          // The private is already gone: either a benign double-cleanup of THIS
          // cb (streamer/thread already reaped -- reap_streamer is a no-op), or
          // this cb LOST a start race (a second start under the same bugname
          // overwrote the private) and nothing else will ever tear it down.
          // Unconditionally skipping the join here orphaned the loser's read
          // thread, which lives in the session pool and dereferenced freed
          // memory after session destroy.
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "%s Bug is not attached (race); reaping this bug's own resources.\n", switch_channel_get_name(channel));
          reap_streamer(cb);
          return SWITCH_STATUS_FALSE;
        }
        switch_channel_set_private(channel, cb->bugname, NULL);

      // stop playback if available
       if (cb->play_file == 1){ 
          if (switch_channel_test_flag(channel, CF_BROADCAST)) {
		        switch_channel_stop_broadcast(channel);
	        } else {
		        switch_channel_set_flag_value(channel, CF_BREAK, 1);
        	}
        }

        // close connection and get final responses (writesDone + join read
        // thread + delete streamer + free resampler/vad)
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "google_speech_session_cleanup: waiting for read thread to complete\n");
        reap_streamer(cb);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "google_speech_session_cleanup: read thread completed\n");

        if (!channelIsClosing) {
          switch_core_media_bug_remove(session, &bug);
        }

			  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "google_speech_session_cleanup: Closed stream\n");

			  // cbLock unlocks cb->mutex here at scope exit
			  return SWITCH_STATUS_SUCCESS;
      }

      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "%s Bug is not attached.\n", switch_channel_get_name(channel));
      return SWITCH_STATUS_FALSE;
    }

    switch_bool_t google_speech_frame(switch_media_bug_t *bug, void* user_data) {
    	switch_core_session_t *session = switch_core_media_bug_get_session(bug);
    	struct cap_cb *cb = (struct cap_cb *) user_data;
		  // single atomic load of streamer; reused for both the gate and the cast so
		  // the pointer cannot change between the null-check and the dereference.
		  GStreamer* streamer = (GStreamer *) cb->streamer.load();
		  if (streamer && (!cb->wants_single_utterance || !cb->got_end_of_utterance)) {
        uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
        switch_frame_t frame = {};
        frame.data = data;
        frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

        if (switch_mutex_trylock(cb->mutex) == SWITCH_STATUS_SUCCESS) {
          // Re-read streamer UNDER the lock. google_speech_session_cleanup deletes
          // the GStreamer and nulls cb->streamer while holding cb->mutex, so the
          // pointer obtained here stays valid for the whole locked region (the
          // pre-lock load at the top of the function is only a fast-path hint and
          // could be stale/freed by a concurrent do_stop). Gating the loop on it
          // means a concurrent delete is observed as NULL -> no use-after-free.
          streamer = (GStreamer *) cb->streamer.load();
          while (streamer && switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS && !switch_test_flag((&frame), SFF_CNG)) {
            if (frame.datalen) {
              if (cb->vad && !streamer->isConnected()) {
                switch_vad_state_t state = switch_vad_process(cb->vad, (int16_t*) frame.data, frame.samples);
                if (state == SWITCH_VAD_STATE_START_TALKING) {
                  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "detected speech, connect to google speech now\n");
                  streamer->connect();
                  cb->responseHandler(session, "vad_detected", cb->bugname);
                }
              }

              if (cb->resampler) {
                spx_int16_t out[SWITCH_RECOMMENDED_BUFFER_SIZE];
                /* interleaved API: capacity and counts are samples PER CHANNEL;
                   out[] holds SWITCH_RECOMMENDED_BUFFER_SIZE elements total */
                spx_uint32_t out_len = SWITCH_RECOMMENDED_BUFFER_SIZE / cb->channels;
                spx_uint32_t in_len = frame.samples;
                size_t written;

                speex_resampler_process_interleaved_int(cb->resampler,
                  (const spx_int16_t *) frame.data,
                  (spx_uint32_t *) &in_len,
                  &out[0],
                  &out_len);
                /* out_len is per-channel samples; bytes = samples * 2 * channels */
                streamer->write( &out[0], sizeof(spx_int16_t) * out_len * cb->channels);
              }
              else {
                /* frame.samples is per-channel (media_bug_read divides by the
                   channel count); frame.datalen is the actual byte count --
                   sizeof(int16) * samples sent only HALF of every stereo frame */
                streamer->write( frame.data, frame.datalen);
              }
            }
          }
          switch_mutex_unlock(cb->mutex);
        }
      }
      return SWITCH_TRUE;
    }
}
