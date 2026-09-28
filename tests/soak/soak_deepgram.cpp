// Concurrency soak of the REAL mod_deepgram_transcribe AudioPipe under
// ASan/TSan. Deepgram's pipe always dials TLS (LCCSCF_USE_SSL), so run.sh
// serves the mock over wss with a throwaway self-signed cert (trusted by the
// client through SSL_CERT_FILE only -- the pipe sets no CA path and no
// skip-verify flag). The handshakes therefore COMPLETE and this exercises the
// connected surface directly:
//   CLIENT_ESTABLISHED (incl. the m_gracefulShutdown record-and-complete),
//   the media-thread ring-buffer writes, the inbound receive path (fragment
//   reassembly + the >650KB MAX_RECV_BUF_SIZE discard, driven by the mock's
//   WS_OVERSIZED_EVERY), CloseStream teardown as driven by the production
//   reaper (finish() + waitForClose(), no close() of its own -- it relies on
//   the remote close), far-end drop, connect-fail, and the destructor /
//   removeFromPending and deinitialize() paths.
//
// g_connects is asserted at the end: if the TLS setup ever regresses to
// CONNECT_FAIL-only, the soak FAILS rather than silently reverting to the
// pre-v0.7.1 state where every "pass" proved nothing about the connected path.
#include "audio_pipe.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using namespace deepgram;

static std::atomic<long> g_events{0};
static std::atomic<long> g_pipes{0};
static std::atomic<long> g_completed{0};
static std::atomic<long> g_connects{0};
static std::atomic<long> g_drops{0};
static std::atomic<long> g_graceful{0};
static std::atomic<long> g_fail{0};
static int g_lmask = LLL_ERR;

static void logger(int level, const char* line) {
  // see soak_audiopipe.cpp: SOAK_LLL widens this mask so the inbound recv-path
  // markers can be confirmed for both pipe lineages
  if (level & g_lmask) fprintf(stderr, "[lws] %s", line);
}
static void onNotify(const char*, AudioPipe::NotifyEvent_t event, const char* message, bool) {
  g_events.fetch_add(1, std::memory_order_relaxed);
  switch (event) {
    case AudioPipe::CONNECT_SUCCESS:
      g_connects.fetch_add(1, std::memory_order_relaxed);
      break;
    case AudioPipe::CONNECTION_DROPPED:
      g_drops.fetch_add(1, std::memory_order_relaxed);
      break;
    case AudioPipe::CONNECTION_CLOSED_GRACEFULLY:
      g_graceful.fetch_add(1, std::memory_order_relaxed);
      break;
    case AudioPipe::CONNECT_FAIL:
      g_fail.fetch_add(1, std::memory_order_relaxed);
      if (message) { volatile size_t n = strlen(message); (void) n; }  // ASan checks the buffer
      break;
    default:
      break;
  }
}

static const char* HOST = "127.0.0.1";
static int PORT = 9000;       // wss mock
static int DROP_PORT = 9001;  // wss mock that drops mid-stream
static int BAD_PORT = 1;      // connection refused -> CONNECT_FAIL
static const char* PATH = "/v1/listen";

static void mediaThread(AudioPipe* ap, std::atomic<bool>* stop) {
  while (!stop->load(std::memory_order_relaxed)) {
    if (ap->getLwsState() == AudioPipe::LWS_CLIENT_CONNECTED) {
      ap->lockAudioBuffer();
      size_t avail = ap->binarySpaceAvailable();
      if (avail < ap->binaryMinSpace()) {
        ap->binaryWritePtrResetToZero();
      } else {
        char* p = ap->binaryWritePtr();
        size_t n = 320;
        if (n <= ap->binarySpaceAvailable()) { memset(p, 0x11, n); ap->binaryWritePtrAdd(n); }
      }
      ap->unlockAudioBuffer();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// identical ownership pattern to the glue's reaper(): finish then wait for the
// remote close, never close()
static void reap(AudioPipe* ap) {
  std::shared_ptr<AudioPipe> sp(ap);
  std::thread([sp]{
    sp->finish();
    sp->waitForClose();
    g_completed.fetch_add(1, std::memory_order_relaxed);
  }).detach();
}

static void oneIteration(int i) {
  int port = PORT;
  if (i % 5 == 2) port = DROP_PORT;   // far-end drop mid-stream
  if (i % 5 == 4) port = BAD_PORT;    // connect-fail

  size_t buflen = LWS_PRE + 320 * 40;
  AudioPipe* ap = new AudioPipe("soak-sess", HOST, (unsigned int)port, PATH,
                                buflen, 640, "fake-api-key", onNotify);
  g_pipes.fetch_add(1, std::memory_order_relaxed);
  ap->connect();

  std::atomic<bool> stop{false};
  std::thread mt(mediaThread, ap, &stop);
  // 3 of 4 iterations stream for a bit; the 4th reaps IMMEDIATELY after
  // connect(), racing finish() against the (now succeeding) TLS handshake --
  // the pre-handshake finish / m_gracefulShutdown record-and-complete path
  if (i % 4 != 3) {
    /* i % 5 == 1 holds long enough for the mock's 700KB oversized message to
       land (the >650KB discard path needs ~82 consecutive 8KB reallocs in one
       pipe); i % 5 == 3 streams a while before the text-send teardown */
    int hold = (i % 5 == 1) ? 250 : ((i % 5 == 3) ? (60 + (int)(i % 40)) : (5 + (int)(i % 25)));
    std::this_thread::sleep_for(std::chrono::milliseconds(hold));
  }
  if (i % 5 == 3) {
    // a text send queued concurrently with teardown: pending-writes vs reaper
    ap->bufferForSending("{\"type\":\"KeepAlive\"}");
  }
  if (i % 4 == 0) ap->finish();  // some explicit early finishes
  stop.store(true, std::memory_order_relaxed);
  mt.join();
  reap(ap);
}

int main() {
  int iters   = getenv("ITER")    ? atoi(getenv("ITER"))    : 200;
  int workers = getenv("WORKERS") ? atoi(getenv("WORKERS")) : 4;
  if (getenv("WS_PORT"))      PORT      = atoi(getenv("WS_PORT"));
  if (getenv("WS_DROP_PORT")) DROP_PORT = atoi(getenv("WS_DROP_PORT"));
  if (getenv("SOAK_LLL"))     g_lmask   = atoi(getenv("SOAK_LLL"));

  /* 2 requested: initialize() caps >1 to 1 at runtime (multi-context connect
     adoption is not thread-safe), so what actually runs is the CAP path with
     a single context -- cross-context discrimination does not execute here */
  AudioPipe::initialize(2 /*requested service threads: exercises the cap*/, g_lmask, logger);
  std::this_thread::sleep_for(std::chrono::seconds(2));

  std::vector<std::thread> ws;
  std::atomic<int> next{0};
  for (int w = 0; w < workers; w++) {
    ws.emplace_back([&]{
      for (;;) {
        int i = next.fetch_add(1);
        if (i >= iters) break;
        oneIteration(i);
      }
    });
  }
  for (auto& t : ws) t.join();

  // let detached reapers drain (the remote close after CloseStream, or
  // CONNECTION_DROPPED / CONNECT_FAIL, fulfills the close promise)
  for (int i = 0; i < 30 && g_completed.load() < iters; i++)
    std::this_thread::sleep_for(std::chrono::seconds(1));

  long completed = g_completed.load();
  long connects  = g_connects.load();
  /* ~4/5 of iterations dial a live mock; requiring a solid majority of those
     to reach CONNECT_SUCCESS is what proves the connected path was covered at
     all. Without this the harness cannot tell a real soak from the old
     TLS-fails-everything one. */
  long need = iters / 5;
  bool ok = completed >= iters && connects >= need;
  fprintf(stderr,
          "%s: %ld/%d reapers completed, %ld CONNECT_SUCCESS (>= %ld required), "
          "%ld dropped, %ld graceful, %ld failed, pipes=%ld events=%ld\n",
          ok ? "REAPER PASS" : "REAPER FAIL",
          completed, iters, connects, need,
          g_drops.load(), g_graceful.load(), g_fail.load(), g_pipes.load(), g_events.load());
  if (completed < iters) fprintf(stderr, "  -> waitForClose leak: %d reapers never returned\n", iters - (int)completed);
  if (connects < need)   fprintf(stderr, "  -> TLS handshakes not completing: connected path NOT covered\n");

  AudioPipe::deinitialize();
  fprintf(stderr, "SOAK DONE\n");
  return ok ? 0 : 1;
}
