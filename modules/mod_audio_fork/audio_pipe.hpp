#ifndef __AUDIO_PIPE_HPP__
#define __AUDIO_PIPE_HPP__

#include <string>
#include <list>
#include <atomic>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <thread>
#include <future>
#include <vector>

#include <libwebsockets.h>

class AudioPipe {
public:
  enum LwsState_t {
    LWS_CLIENT_IDLE,
    LWS_CLIENT_CONNECTING,
    LWS_CLIENT_CONNECTED,
    LWS_CLIENT_FAILED,
    LWS_CLIENT_DISCONNECTING,
    LWS_CLIENT_DISCONNECTED
  };
  enum NotifyEvent_t {
    CONNECT_SUCCESS,
    CONNECT_FAIL,
    CONNECTION_DROPPED,
    CONNECTION_CLOSED_GRACEFULLY,
    MESSAGE
  };
  typedef void (*log_emit_function)(int level, const char *line);
  typedef void (*notifyHandler_t)(const char *sessionId, const char* bugname, NotifyEvent_t event, const char* message);

  struct lws_per_vhost_data {
    struct lws_context *context;
    struct lws_vhost *vhost;
    const struct lws_protocols *protocol;
  };

  static void initialize(const char* protocolName, unsigned int nThreads, int loglevel, log_emit_function logger);
  static bool deinitialize();
  static bool lws_service_thread(unsigned int nServiceThread);

  // constructor
  AudioPipe(const char* uuid, const char* host, unsigned int port, const char* path, int sslFlags, 
    size_t bufLen, size_t minFreespace, const char* username, const char* password, char* bugname, notifyHandler_t callback);
  ~AudioPipe();  

  LwsState_t getLwsState(void) { return m_state; }
  void connect(void);
  void bufferForSending(const char* text);
  size_t binarySpaceAvailable(void) {
    return m_audio_buffer_max_len - m_audio_buffer_write_offset;
  }
  size_t binaryMinSpace(void) {
    return m_audio_buffer_min_freespace;
  }
  char * binaryWritePtr(void) { 
    return (char *) m_audio_buffer + m_audio_buffer_write_offset;
  }
  void binaryWritePtrAdd(size_t len) {
    m_audio_buffer_write_offset += len;
  }
  void binaryWritePtrResetToZero(void) {
    m_audio_buffer_write_offset = LWS_PRE;
  }
  void lockAudioBuffer(void) {
    m_audio_mutex.lock();
  }
  void unlockAudioBuffer(void) ;
  bool hasBasicAuth(void) {
    return !m_username.empty() && !m_password.empty();
  }

  void getBasicAuth(std::string& username, std::string& password) {
    username = m_username;
    password = m_password;
  }

  void do_graceful_shutdown();
  bool isGracefulShutdown(void) {
    return m_gracefulShutdown;
  }

  void close() ;

  // Promise-based close signalling. The lws CLOSED / CONNECT_FAIL paths call
  // setClosed() (set-once); the reaper blocks in waitForClose() before deleting
  // the AudioPipe, so the object always outlives any lws-thread use of it.
  // This replaces the previous (racy) delete-inside-the-lws-callback model.
  void setClosed();
  void waitForClose();

  // no default constructor or copying
  AudioPipe() = delete;
  AudioPipe(const AudioPipe&) = delete;
  void operator=(const AudioPipe&) = delete;

private:

  static int lws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len); 
  static std::atomic<unsigned int> nchild;
  /* written by each service thread at startup (lws_create_context) and read
     by connecting threads (addPendingConnect/addPendingWrite wake targets);
     atomic slots give those cross-thread accesses a happens-before edge */
  static std::atomic<struct lws_context*> contexts[];
  static unsigned int numContexts;
  static std::string protocolName;
  static std::mutex mutex_connects;
  static std::mutex mutex_disconnects;
  static std::mutex mutex_writes;
  static std::list<AudioPipe*> pendingConnects;
  static std::list<AudioPipe*> pendingDisconnects;
  static std::list<AudioPipe*> pendingWrites;

  /* shutdown coordination: deinitialize() sets stopRequested, wakes each lws
     context, and JOINs the (non-detached) service threads before destroying the
     contexts, so no service thread races teardown. */
  static std::atomic<bool> stopRequested;
  static std::vector<std::thread> serviceThreads;

  static AudioPipe* findAndRemovePendingConnect(struct lws *wsi);
  static AudioPipe* findPendingConnect(struct lws *wsi);
  static void addPendingConnect(AudioPipe* ap);
  static void addPendingDisconnect(AudioPipe* ap);
  static void addPendingWrite(AudioPipe* ap);
  /* Remove ap from every pending list under the list mutexes. Called from the
     destructor so a reaped pipe cannot be dereferenced by the lws service thread
     (which reads (*it)->m_state under those same mutexes) after it is freed. */
  static void removeFromPending(AudioPipe* ap);
  static void processPendingConnects(lws_per_vhost_data *vhd);
  static void processPendingDisconnects(lws_per_vhost_data *vhd);
  static void processPendingWrites(lws_per_vhost_data *vhd);
  
  bool connect_client(struct lws_per_vhost_data *vhd);

  /* connection-state flag shared between the lws service thread and the
     media-bug threads; atomic so reads/writes don't race (seq_cst) */
  std::atomic<LwsState_t> m_state;
  std::string m_uuid;
  std::string m_host;
  std::string m_bugname;
  unsigned int m_port;
  std::string m_path;
  std::string m_metadata;
  std::mutex m_text_mutex;
  std::mutex m_audio_mutex;
  int m_sslFlags;
  struct lws *m_wsi;
  uint8_t *m_audio_buffer;
  size_t m_audio_buffer_max_len;
  size_t m_audio_buffer_write_offset;
  size_t m_audio_buffer_min_freespace;
  uint8_t* m_recv_buf;
  uint8_t* m_recv_buf_ptr;
  size_t m_recv_buf_len;
  /* true while dropping the remaining fragments of an inbound message that was
     abandoned mid-delivery (oversized, or its buffer allocation failed); only
     ever touched on the lws service thread */
  bool m_recv_buf_discarding;
  struct lws_per_vhost_data* m_vhd;
  notifyHandler_t m_callback;
  std::string m_username;
  std::string m_password;
  /* cross-thread flag (written by graceful-shutdown path, read by lws callback) */
  std::atomic<bool> m_gracefulShutdown;
  /* a close() that arrived while the WS handshake was still in flight; completed
     by LWS_CALLBACK_CLIENT_ESTABLISHED so the reaper's waitForClose() cannot
     block forever on a connection nothing will ever close */
  std::atomic<bool> m_closePending;
  /* set-once guard so the close promise is fulfilled exactly once regardless of
     which terminal path (graceful close, far-end drop, or connect failure) runs */
  std::atomic<bool> m_closeSignaled;
  std::promise<void> m_promise;
};

#endif
