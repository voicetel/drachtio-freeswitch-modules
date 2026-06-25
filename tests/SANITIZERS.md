# Sanitizing the modules (ASan / UBSan / TSan)

There are two distinct things, and they cover different code:

## 1. Host harness — runnable anywhere (no FreeSWITCH, no creds)

`make -C tests sanitize` builds the FreeSWITCH-independent units (`base64`,
`SimpleBuffer`) with **ASan + UBSan** and runs them. This catches buffer
overruns, use-after-free, and undefined behavior **in that logic only**. It does
**not** touch the modules' threading — `SimpleBuffer` has no internal locking
(callers lock), so it is single-threaded here and TSan adds nothing.

Status: clean.

## 2. The modules' threading — needs a live FreeSWITCH + a real call

The media-bug thread, the websocket/gRPC/SDK threads, and the teardown/reaper
path only run concurrently during an actual transcription call. So real ASan/TSan
results require **building the module with sanitizers inside the FreeSWITCH tree
and exercising it with live calls** (vendor credentials required). Loading the
module without a call exercises ~none of the concurrency — TSan would report
"clean" trivially (a false negative). Do not trust a sanitizer run that didn't
make calls.

### Build flags

Append to the per-module compile/link in the installer's `install_mod_*` recipe
(`callBroadcast/freeswitch/install-freeswitch-debian-trixie.sh`):

- **ASan + UBSan** (memory safety, UAF, UB):
  `-fsanitize=address,undefined -fno-omit-frame-pointer -g`
  (add to both the per-file compiles and the final `-shared` link).
- **TSan** (data races) — build the module with:
  `-fsanitize=thread -fno-omit-frame-pointer -g`
  TSan is most reliable when the **whole process** is instrumented; ideally also
  build FreeSWITCH (and libwebsockets) with `-fsanitize=thread`. With only the
  module instrumented you still get reports for races that touch module code, but
  expect some noise and false negatives across the un-instrumented boundary.

Do ASan and TSan in **separate** builds (they are mutually exclusive).

### Running

Start FreeSWITCH so the sanitizer runtime loads with the module. If FS itself was
not built with the sanitizer, preload it:

    ASAN_OPTIONS=detect_leaks=1:halt_on_error=0:log_path=/tmp/asan \
      LD_PRELOAD=$(gcc -print-file-name=libasan.so) \
      /usr/local/freeswitch/bin/freeswitch -nonat -nc

    # TSan build:
    TSAN_OPTIONS=halt_on_error=0:second_deadlock_stack=1:log_path=/tmp/tsan \
      /usr/local/freeswitch/bin/freeswitch -nonat -nc

### Scenarios to exercise (the soak)

Per vendor (deepgram/google/aws/azure — needs that vendor's creds) and for
mod_audio_fork (needs a ws sink), run each and watch /tmp/asan.* and /tmp/tsan.*:

1. Start transcription, stream audio, normal stop.
2. **Far-end drop mid-call** (kill the vendor/ws connection while audio flows).
3. **Graceful stop** while streaming.
4. **Hangup mid-call** (channel teardown while connected) — exercises the reaper.
5. **Rapid start/stop** churn.
6. **Connect failure** (bad host/creds) — exercises the CONNECT_FAIL path.
7. Sustained back-pressure (aws): confirm the bounded deque drops oldest
   (`AWS_TRANSCRIBE_MAX_BUFFERED_FRAMES`) rather than growing.

### Caveats

- FreeSWITCH's pool allocator can mask some heap errors from ASan; module-owned
  `new`/`malloc` are still covered.
- Sanitizer builds are slow and memory-hungry — not for production; a dev/staging
  FS host.
- Static substitute (no run): `clang-tidy` with `clang-analyzer-*` /
  `bugprone-*` / `concurrency-*` checks, run inside the FS tree (so headers
  resolve), plus `cppcheck`. These find some issues statically but are not a
  replacement for a TSan run under live calls.
