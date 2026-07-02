# Changelog

Changes since VoiceTel took over maintenance of this fork (baseline:
`aa07b27`, "README: mark fork as voicetel-maintained").

Scope of maintenance: the four streaming speech-to-text modules used by
callBroadcast — `mod_deepgram_transcribe`, `mod_google_transcribe`,
`mod_aws_transcribe`, `mod_azure_transcribe` — plus `mod_audio_fork`. All other
modules are inherited from upstream and unmaintained.

Verification legend: **[unit]** host unit tests + ASan/UBSan, **[build]** compiles/
links/loads in FreeSWITCH 1.10.12, **[tsan]** ThreadSanitizer-clean via
`tests/soak`. The vendor-SDK streaming paths and the FS glue still require a
live-credentials soak (see `docs/TESTING.md`).

---

## v0.6.0 — 2026-07-02

Full adversarial code review of all four maintained transcribe modules
(~5,300 lines, every source file), one commit per issue: **52 confirmed
findings fixed across 45 commits** (deepgram 14, aws 10, azure 9, google 9,
plus 1 cross-module and 2 follow-ups the new soak itself surfaced).
**[unit] [build] [tsan]**

Highlights by severity (per-issue detail is in the individual commit messages):

- **Crashes / use-after-free:**
  - aws: a stop racing the async connect deleted the GStreamer while the SDK
    still held `this`-capturing callbacks (UAF on an SDK thread); a stop during
    GStreamer construction hung the hangup thread forever (VAD path).
  - azure: env-var-only auth passed a NULL subscription key to
    `SpeechConfig::FromSubscription` (`std::string(nullptr)` — segfault on the
    first start); same for an unauthenticated proxy's NULL username/password;
    the init-failure path deleted the GStreamer with all six raw-`this` SDK
    handlers still connected; a throwing `StopContinuousRecognitionAsync().get()`
    on the detached reaper thread was `std::terminate`.
  - google: bug-add failure and lost same-bugname start races orphaned the
    gRPC read thread past session destroy (session-pool UAF); an invalid
    JWT key in the `GOOGLE_APPLICATION_CREDENTIALS` channel variable
    dereferenced a null credentials pointer; a non-string hints phrase
    crashed protobuf `set_value`.
  - deepgram: a NULL lws connect-error string was streamed into an
    ostringstream on the shared lws service thread; a stereo resample could
    overflow the send buffer (capacity math ignored channels — same fix in
    aws/azure/google's declared capacities); `finish()` racing the WS
    handshake leaked the pipe, thread, and socket forever (ttsd v0.3.0 port);
    media-bug-add failure leaked a connecting AudioPipe (same fix in azure).
- **gRPC/lws API contract:** google's `writesDone()` TOCTOU double half-close;
  `Finish()` unserialized against concurrent `Write()`; writes after
  half-close. deepgram's `lws_write` failure hidden by a signed/unsigned
  comparison; cross-context `lws_callback_on_writable` calls with >1 service
  thread (and, found by this release's new soak: an unsynchronized
  `contexts[]` publish and a cross-context connect-adoption race — service
  threads are now capped at 1, the default and production configuration,
  until adoption is per-context).
- **Silent failure / audio loss:** google surfaced only OUT_OF_RANGE stream
  failures — bad credentials/UNAVAILABLE/etc. produced silence (now evented);
  azure's unsolicited SessionStopped and push-stream write failures were
  invisible to the consumer (now evented via the existing error subclass);
  aws's prebuffer drain sent only the first half of every chunk on resampled
  calls; pre-connect audio was dropped wholesale for non-320-multiple frames
  in aws/azure/google (30ms ptime, resampler output); stereo audio was sent
  at half its bytes (aws/google) or resampled as mono (aws/azure); deepgram's
  empty-transcript filter could discard payloads containing real content.
- **Hangs:** google had no deadline/keepalive anywhere — a silent network
  partition stalled hangup for the TCP failure-detection duration (HTTP/2
  keepalive added, 60s/20s, active calls only); aws's worker held its mutex
  across blocking `WriteAudioEvent` calls, stalling the FS media thread under
  back-pressure and making the v0.4.0 bounded-deque cap unreachable.
- **Consistency/robustness:** bugname key-truncation orphans (aws/azure/google
  — over-length names now rejected at the API); deepgram stop cleared the
  wrong channel-private key and never populated the event `media-bugname`
  header; connect-transition frame reordering/stranding (aws/azure); worker
  thread exception guards (aws); event-subclass reserve/free (aws);
  NULL-`%s` log arguments and deref-before-NULL-check cleanups (all four).

Event subclass names and event-body JSON shapes are unchanged throughout;
the only consumer-visible event changes are additive (previously-missing
error events, and deepgram's `media-bugname` header now carrying the real
bug name instead of an empty string).

- **Tests:** `tests/soak` now also soaks the deepgram AudioPipe (ASan+UBSan +
  TSan; TLS-only dialer, so it exercises the connect-fail/teardown/reaper
  surfaces — the receive-path code is byte-identical to the
  mod_ttsd_transcribe AudioPipe, whose own soak covers it end-to-end over
  plain ws). lws-internal logging races are suppressed via `tsan.supp`
  (library-only frames; module frames are never suppressed).

### Deferred (recorded, not fixed)
- aws: the shutdown wait for the SDK's final response is unbounded on a dead
  network (no safe timeout without SDK-internal verification), and
  `m_finished` is set inside `OnResponseCallback` before the SDK's async
  frame fully unwinds — whether the post-callback unwind touches the client
  is SDK-internal (needs aws-sdk-cpp source review or a live-creds TSan soak).
- google: the VAD-path `connect()` still blocks the FS media thread on
  network I/O under `cb->mutex` (structural; keepalive bounds the worst case).
- Multi-context lws service threads stay capped at 1 pending a per-context
  connect-adoption redesign (also applies to the mod_audio_fork /
  mod_ttsd_transcribe lineage — not yet ported there).

### Verification
- **[unit]** `make -C tests coverage`: 9/9 host tests pass, 100% line coverage
  of the FS-independent units (`base64.hpp`, `simple_buffer.h` — the latter
  unchanged by these fixes).
- **[build]** All four modules rebuilt from the fixed sources with the exact
  callBroadcast-installer recipes against FreeSWITCH 1.10.12 in Docker
  (aws-sdk-cpp static stack, Speech SDK 1.49.0, googleapis gens):
  compile + link clean, `module_exists` = true for all four, 5 transcribe
  APIs registered. The only warning is a pre-existing `speaker_tag`
  deprecation in inherited google code.
- **[tsan]** `tests/soak` (both AudioPipes, ASan+UBSan+LSan and TSan builds):
  OVERALL PASS; deepgram reaper completion 200/200 in both builds; 0 TSan
  warnings across 5 additional stress runs (300 iterations × 6 workers).
  The soak itself found two real races during this release (fixed above:
  `contexts[]` publish; capped multi-context mode).
- **Not verified here:** live-credential vendor streaming (deepgram/google/
  aws/azure runtime transcription needs real credentials + audio — see
  `docs/TESTING.md`); the FS glue under concurrent live calls.

## v0.5.2 — 2026-06-28 (`7551c4b`, `0925fe1`)
AudioPipe teardown use-after-free fix (`mod_audio_fork`, `mod_deepgram_transcribe`).
**[build] [tsan]**

- **The lws service thread reads `(*it)->m_state` for every entry in the pending
  connect/disconnect/write lists (under each list's mutex), but the reaper could
  `delete` an AudioPipe still present in `pendingWrites`** — a write queued by the
  media thread's last `unlockAudioBuffer` just before teardown. Result: a
  heap-use-after-free read of `m_state` on the lws thread. → `~AudioPipe()` now
  calls `removeFromPending(this)`, removing the pipe from all three lists under
  those lists' mutexes, so the read is serialized with the free (present-and-alive,
  or already-gone — never read after free; locks taken one at a time, no deadlock).
- Found reproducibly under **ThreadSanitizer** in the structurally-identical
  `mod_ttsd_transcribe` AudioPipe (a fork of this code, in its own repo); the fix
  is identical across all three. `mod_audio_fork`'s `tests/soak` (ASan + TSan,
  200×4 scenarios) stays clean with the fix. It did not reproduce on that soak's
  `close()`-based reaper (narrower window), so for the in-repo modules this is a
  defensive backport of a verified fix.

## v0.5.1 — 2026-06-25 (`65f2a45`)
Docs only.
- Added `docs/TESTING.md`: the reproducible 5-layer testing methodology, gotchas,
  a new-module decision tree, and an honesty checklist. README links the testing
  layers.

## v0.5.0 — 2026-06-25 (`9aac6db`, `67c2b9c`)
Full data-race audit of every maintained module, plus a race-free shutdown and an
ASan/TSan soak harness. **[unit] [build] [tsan]**

- **Cross-thread struct flags were plain `int`/bitfields read on the media thread
  while written by another thread** (`got_end_of_utterance` in google;
  `audio_paused`/`graceful_shutdown` in mod_audio_fork) → made atomic via an
  ABI-safe `#ifdef __cplusplus std::atomic` pattern (C view/struct layout
  unchanged). A `cb->mutex` guard was rejected for `got_end_of_utterance` because
  it deadlocks cleanup (holds the mutex across `thread_join`) vs the read thread.
- **Cross-thread stream pointers published without synchronization**
  (`cb->streamer` in google/aws) → `std::atomic<void*>`.
- **google: `Write` (media thread) and `WritesDone` (read thread) on the same gRPC
  stream were not serialized** (undefined behavior) → guarded by a dedicated write
  mutex.
- **google: `google_speech_frame` loaded `cb->streamer` before the trylock and
  dereferenced the stale local under the lock** → use-after-free on the do_stop
  path (cleanup deletes+nulls the stream under the mutex). Now re-reads under the
  lock and gates the read loop on it.
- **aws/azure: the pre-connect prebuffer (`SimpleBuffer`) was added-to on the media
  thread and drained on an SDK thread without synchronization** → guarded by a
  dedicated buffer mutex (one-way lock order, non-recursive-safe, no deadlock).
- **mod_audio_fork: the `playout` temp-file list was appended on the lws thread and
  freed on the API thread** → append now under `tech_pvt->mutex` (matches the free
  loop).
- **AudioPipe shutdown was racy** (`deinitialize`): service threads were detached,
  the service loop read a `std::map` (`stopFlags[this_id]`) every iteration without
  the mutex, and only one of N threads was stopped before contexts were destroyed.
  Found by ThreadSanitizer. → replaced with an atomic stop flag + joinable threads;
  `deinitialize` now signals stop, `lws_cancel_service`s each context, **joins**
  the threads, then destroys the contexts.
- **Benign global counters** (`idxCallCount`, `AudioPipe::nchild`) raced on
  concurrent `start` → made atomic.
- Added `tests/soak/`: a Dockerized ASan+TSan concurrency soak of the real
  `AudioPipe` (200×4 iterations: connect / stream / far-end drop / graceful stop /
  rapid restart / connect-fail / teardown / shutdown) — result: ASan + TSan clean.

## v0.4.0 — 2026-06-25 (`e6239b4`; tooling `eaedb7d`)
The concurrency/lifecycle items flagged-but-deferred in the v0.1.0 review.
**[unit] [build]**
- **deepgram: detached `reaper` thread captured `tech_pvt` and read it after the
  session could be freed** (use-after-free); **`deinitialize`-era `switch_mutex_destroy`
  of a session-pool-owned mutex** (double cleanup) → capture `sessionId`/`id` by
  value; stop destroying the pool mutex; close-promise signalled on every terminal
  path so the reaper can't hang.
- **aws: `m_pStream` published on the SDK IO thread without the mutex** → atomic;
  **unbounded audio deque under back-pressure** → bounded with drop-oldest
  (`AWS_TRANSCRIBE_MAX_BUFFERED_FRAMES`, default ~10s) [behavior-changing];
  **thread + GStreamer leaked when `switch_core_media_bug_add` failed after init**
  → teardown on that path; **hot path:** per-frame heap alloc moved out of the lock.
- **google: gRPC write-side `Write`/`WritesDone` not serialized** → write mutex
  (refined in v0.5.0).
- **azure: SDK callbacks could fire on a GStreamer being torn down** → `DisconnectAll()`
  every recognizer signal before `StopContinuousRecognitionAsync().get()`.
- Tooling: `make -C tests sanitize` (ASan/UBSan) + `tests/SANITIZERS.md`.

## v0.3.0 — 2026-06-24 (`fdf245a`)
`mod_audio_fork` AudioPipe use-after-free rework. **[build]**, since soak-tested live.
- **The AudioPipe was `delete`d inside the lws CLOSED callback while a media-bug
  thread could still hold it, and `eventCallback` cleared `pAudioPipe` without the
  mutex** → genuine use-after-free on far-end drop during an active call. Replaced
  with promise-based close signalling (`setClosed`/`waitForClose`) and a reaper that
  deletes the pipe only after the media bug is removed and the socket close is
  signalled. (`f84d7dc`: dropped the README "hardening in progress" caveat after the
  live soak confirmed it.)

## v0.2.0 — 2026-06-24 (`15861ee`; `0c8d0e5`)
`mod_audio_fork` safe-tier hardening (ported from its `mod_deepgram_transcribe`
descendant). **[build]**
- LWS_PRE ring-buffer reset (was reset to `0`, shipping header bytes / corrupting
  audio on overrun); NULL-`ap` dereferences in log branches; stereo resampler byte
  math; `lws_logger` level mapping; atomic connection state; auth fail-closed (was
  silently connecting unauthenticated on gen failure); VLA→heap for a
  server-controlled length; cJSON/URI NUL guards; RAII guards. (`0c8d0e5`: README
  marks `mod_audio_fork` maintained.)

## v0.1.0 — 2026-06-24 (`55ad536`; `f21b8de`; `e2b3298`)
First tagged release: the four transcribe modules. **[unit] [build]**
- **Memory-safety / input-validation:** `simple_buffer.h` (shared, byte-identical)
  read via the *write* cursor and underflowed `getNextChunk()` → separate read
  cursor + empty guard; `memset(cb, sizeof(cb), 0)` (zeroed nothing) in aws/azure
  → `memset(cb, 0, sizeof(*cb))`; `argv[1]` dereferenced before the `argc` check in
  all four API handlers (NULL-deref crash) → guarded; unterminated credential
  `strncpy`; deepgram LWS_PRE reset, NULL-`ap` logs, api-key bound, resampler math,
  `&version=`; defensive `responseHandler` (no event-subclass renames — callBroadcast
  consumes those).
- **Tier 1+2 optimizations:** aws transcript JSON via cJSON (correct escaping,
  identical shape); google `const&` protobuf loop vars + lazy VAD buffer; azure
  `640`-vs-`320` prebuffer fix; INFO→DEBUG transcript logging; RAII session/mutex
  guards; magic-number naming.
- Added `tests/` host unit harness (`base64`, `SimpleBuffer`, 100% line coverage).
- (`e2b3298`: README rewritten to state the maintained scope.)
