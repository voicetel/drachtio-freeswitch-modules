// Standalone concurrency soak of the REAL mod_audio_fork AudioPipe.
// Drives, concurrently and in a loop: the lws service thread (started by
// AudioPipe::initialize), a "media thread" that writes audio into the ring buffer
// exactly like fork_frame does, and the teardown/reaper sequence
// (close -> waitForClose -> delete via shared_ptr on a detached thread, capturing
// by value) exactly like fork_session_cleanup. Build under ASan or TSan to catch
// use-after-free / heap errors / data races in the actual AudioPipe code.
//
// No FreeSWITCH: AudioPipe depends only on libwebsockets + std + a callback ptr.
#include "audio_pipe.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

static std::atomic<long> g_events{0};
static std::atomic<long> g_pipes{0};

static void logger(int level, const char* line) {
  // keep noise down; surface only lws errors
  if (level == LLL_ERR) fprintf(stderr, "[lws] %s", line);
}
static void onNotify(const char*, const char*, AudioPipe::NotifyEvent_t, const char*) {
  g_events.fetch_add(1, std::memory_order_relaxed);
}

static const char* HOST = "127.0.0.1";
static int PORT = 9000;       // normal mock ws
static int DROP_PORT = 9001;  // mock ws that drops mid-stream
static int BAD_PORT = 1;      // connect-fail

// media thread: mimic fork_frame writing 320-byte L16 frames into the ring buffer
static void mediaThread(AudioPipe* ap, std::atomic<bool>* stop) {
  while (!stop->load(std::memory_order_relaxed)) {
    if (ap->getLwsState() == AudioPipe::LWS_CLIENT_CONNECTED) {
      ap->lockAudioBuffer();
      size_t avail = ap->binarySpaceAvailable();
      if (avail < ap->binaryMinSpace()) {
        ap->binaryWritePtrResetToZero();        // overrun path (LWS_PRE reset)
      } else {
        char* p = ap->binaryWritePtr();
        size_t n = 320;
        if (n <= ap->binarySpaceAvailable()) { memset(p, 0x11, n); ap->binaryWritePtrAdd(n); }
      }
      ap->unlockAudioBuffer();                  // queues a pending write
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// reaper: identical ownership pattern to fork_session_cleanup (capture by value;
// detached thread closes, waits for the socket-closed promise, then deletes).
static void reap(AudioPipe* ap) {
  std::shared_ptr<AudioPipe> sp(ap);
  std::thread([sp]{ sp->close(); sp->waitForClose(); }).detach();
}

static void oneIteration(int i) {
  int port = PORT;
  if (i % 5 == 2) port = DROP_PORT;   // far-end drop scenario
  if (i % 5 == 4) port = BAD_PORT;    // connect-fail scenario

  char bug[] = "soakbug";
  size_t buflen = LWS_PRE + 320 * 40;
  AudioPipe* ap = new AudioPipe("soak-sess", HOST, (unsigned int)port, "/", 0 /*ssl*/,
                                buflen, 640 /*minfree*/, "", "", bug, onNotify);
  g_pipes.fetch_add(1, std::memory_order_relaxed);
  ap->connect();

  std::atomic<bool> stop{false};
  std::thread mt(mediaThread, ap, &stop);
  std::this_thread::sleep_for(std::chrono::milliseconds(20 + (i % 30)));

  switch (i % 5) {
    case 0: ap->do_graceful_shutdown(); break;          // graceful (zero-len frame)
    case 3: ap->bufferForSending("{\"type\":\"x\"}"); break; // text + rapid teardown
    default: break;                                      // close/drop/connect-fail
  }
  // In real FS the media bug is removed (media stops) before the reaper runs.
  stop.store(true, std::memory_order_relaxed);
  mt.join();
  reap(ap);   // hand off ownership; detached close+waitForClose+delete
}

int main(int argc, char** argv) {
  int iters   = getenv("ITER")    ? atoi(getenv("ITER"))    : 200;
  int workers = getenv("WORKERS") ? atoi(getenv("WORKERS")) : 4;
  if (getenv("WS_PORT"))      PORT      = atoi(getenv("WS_PORT"));
  if (getenv("WS_DROP_PORT")) DROP_PORT = atoi(getenv("WS_DROP_PORT"));

  AudioPipe::initialize("audio.drachtio.org", 1 /*service threads*/, LLL_ERR, logger);
  // wait for the service thread to create the lws context before any connect
  // (in FreeSWITCH, initialize runs at module load, long before the first call)
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

  // let detached reapers drain (waitForClose returns on CLOSED/CONNECT_FAIL)
  std::this_thread::sleep_for(std::chrono::seconds(3));
  AudioPipe::deinitialize();

  fprintf(stderr, "SOAK DONE: pipes=%ld events=%ld iters=%d workers=%d\n",
          g_pipes.load(), g_events.load(), iters, workers);
  return 0;
}
