// Concurrency soak of the REAL mod_deepgram_transcribe AudioPipe under
// ASan/TSan. Deepgram's pipe always dials TLS (LCCSCF_USE_SSL), so against a
// plain-ws mock or a refused port every connect terminates via
// LWS_CALLBACK_CLIENT_CONNECTION_ERROR -> CONNECT_FAIL -> setClosed(). That
// still concurrently exercises: addPendingConnect (round-robin + NULL-context
// guard), findAndRemovePendingConnect, finish() pre-handshake
// (m_gracefulShutdown path), the reaper promise, ~AudioPipe()
// removeFromPending, and deinitialize() -- the teardown surfaces changed in
// this release. The RECEIVE-path changes are exercised by the identical
// mod_ttsd_transcribe soak (ws, no TLS) in that repo.
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

static void logger(int level, const char* line) {
  if (level == LLL_ERR) fprintf(stderr, "[lws] %s", line);
}
static void onNotify(const char*, AudioPipe::NotifyEvent_t event, const char* message, bool) {
  g_events.fetch_add(1, std::memory_order_relaxed);
  if (event == AudioPipe::CONNECT_FAIL && message) {
    // touch the message (ASan checks the buffer) -- may legitimately be NULL
    volatile size_t n = strlen(message);
    (void) n;
  }
}

static const char* HOST = "127.0.0.1";
static int PORT = 9000;       // plain-ws mock: TLS handshake fails -> CONNECT_FAIL
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

// identical ownership pattern to the glue's reaper()
static void reap(AudioPipe* ap) {
  std::shared_ptr<AudioPipe> sp(ap);
  std::thread([sp]{
    sp->finish();
    sp->waitForClose();
    g_completed.fetch_add(1, std::memory_order_relaxed);
  }).detach();
}

static void oneIteration(int i) {
  int port = (i % 3 == 2) ? BAD_PORT : PORT;
  size_t buflen = LWS_PRE + 320 * 40;
  AudioPipe* ap = new AudioPipe("soak-sess", HOST, (unsigned int)port, PATH,
                                buflen, 640, "fake-api-key", onNotify);
  g_pipes.fetch_add(1, std::memory_order_relaxed);
  ap->connect();

  std::atomic<bool> stop{false};
  std::thread mt(mediaThread, ap, &stop);
  if (i % 4 != 3) {
    // 3 of 4 iterations wait a bit; the 4th reaps IMMEDIATELY after connect(),
    // racing finish() against the (failing) handshake -- the pre-handshake
    // finish/m_gracefulShutdown path plus CONNECT_FAIL's own setClosed()
    std::this_thread::sleep_for(std::chrono::milliseconds(5 + (i % 25)));
  }
  if (i % 4 == 0) ap->finish();  // some explicit early finishes
  stop.store(true, std::memory_order_relaxed);
  mt.join();
  reap(ap);
}

int main() {
  int iters   = getenv("ITER")    ? atoi(getenv("ITER"))    : 200;
  int workers = getenv("WORKERS") ? atoi(getenv("WORKERS")) : 4;
  if (getenv("WS_PORT")) PORT = atoi(getenv("WS_PORT"));

  AudioPipe::initialize(2 /*service threads: exercise the per-context filtering*/, LLL_ERR, logger);
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

  // let detached reapers drain (CONNECT_FAIL fulfills the close promise)
  for (int i = 0; i < 30 && g_completed.load() < iters; i++)
    std::this_thread::sleep_for(std::chrono::seconds(1));

  long completed = g_completed.load();
  bool ok = completed >= iters;
  fprintf(stderr, "%s: %ld/%d reapers completed (pipes=%ld events=%ld)\n",
          ok ? "REAPER PASS" : "REAPER FAIL (waitForClose leak)", completed, iters, g_pipes.load(), g_events.load());

  AudioPipe::deinitialize();
  fprintf(stderr, "SOAK DONE\n");
  return ok ? 0 : 1;
}
