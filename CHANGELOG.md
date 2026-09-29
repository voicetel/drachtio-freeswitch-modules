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

## Unreleased

Third full review of the maintained surface (all five modules), one commit per
issue. Headline: a cross-module ABBA deadlock none of the prior reviews
caught, plus a crash-class teardown race in each of audio_fork and aws.

### mod_audio_fork
- **Stop/hangup ABBA deadlock** — `fork_session_cleanup` held `tech_pvt->mutex`
  across `switch_core_media_bug_remove` (which takes `session->bug_rwlock` in
  write mode), while the hangup path (`switch_core_media_bug_remove_all` at
  session-thread exit) holds that write lock across the module's CLOSE
  callback, which takes `tech_pvt->mutex`. A stop racing a hangup wedged both
  threads permanently. The bug is now removed after the mutex is released
  (verified safe against FS 1.10.12: the CLOSE fired by remove early-returns
  on the cleared private; a remove on an already-unlinked bug is a not-found
  walk on session-pool memory).
- **Concurrent cleanup crashed the process** — two in-flight cleanups (two API
  stops share the session read lock, or stop racing hangup CLOSE) double-freed
  the playout list, sent into a reaper-deleted pipe, and reaped a NULL pipe
  (`shared_ptr<AudioPipe>(nullptr)` → null deref on the detached thread).
  Cleanup is now idempotent (bails when the private is already cleared, NULLs
  the playout head, reads the pipe pointer under the lock).
- **Stereo `media_bug_read` heap overflow on the non-resampled path** — the
  direct ring-buffer read guarded on the mono `binaryMinSpace()` while FS's
  `SMBF_STEREO` path writes 2x that; reachable at native rate when
  2·bytes_per_packet does not divide the ring size (e.g. stereo PCMU at 30ms
  ptime). The v0.7.0 stereo hardening covered only the resampler branch. The
  direct-path minimum free space is now channel-scaled. (Deepgram lineage
  copy fixed in its own commit.)
- **`start` on a codec-less session dereferenced a NULL read codec**
  (pre-answer'd outbound leg without early media) — now fails the start.
- **Invalid `ws-uri`/sample-rate arguments still ran `start_capture`** (with
  uninitialized `host/path/port/sslFlags`) — validation now gates the call.
- **Unbounded `sampling` argument overflowed the ring-size math** (`320 ·
  sampling` as int → `bad_alloc` → `std::terminate`; also memory-exhaustion
  sizing). Bounded to 1..48000, multiple of 8000.
- **`send_text`/`pauseresume`/`graceful-shutdown` used the pipe/bug
  unsynchronized against teardown** — the un-fixed siblings of `11034b5`; they
  now take `tech_pvt->mutex` and re-verify the private under it.
- **Over-length bugname orphaned hangup cleanup** (private keyed by full name,
  CLOSE looked up the truncated copy → permanent pipe+connection leak) —
  rejected at the API, matching aws/azure/google.
- **Concurrent same-bugname `start`s orphaned the first pipe** — FS only
  rejects duplicate bug names with `SMBF_ONE_ONLY`; now passed.
- **Unload with live sessions destroyed the lws context under them**
  (connecting pipes' reapers then blocked in `waitForClose()` forever, and
  detached reapers could run module code on unmapped text) — a live-pipe
  counter now refuses the unload, matching azure/aws.
- **Oversized single-frame inbound messages bypassed `MAX_RECV_BUF_SIZE`**
  (the cap was only checked in the realloc branch; a first-fragment length is
  allocated whole) — now capped at first-fragment time.
- Chore: dead `fork_service_threads` decl / dead connect-failure branch /
  dead null-check after `new`; NULL-`%s` log args; `-Wreorder` ctor init list;
  unused lws local.

### mod_deepgram_transcribe
- **Stop/hangup ABBA deadlock** — same class as audio_fork (mutex held across
  `switch_core_media_bug_remove` vs hangup CLOSE under `bug_rwlock`); the bug
  is now removed after the mutex is released.
- **A second concurrent stop could clear a NEW session's private slot** —
  `dg_transcribe_session_stop` cleared the `MY_BUG_NAME` private
  unconditionally; a stop racing a stop+start cycle would orphan the new
  session's stop path. The private is now re-verified under the lock (as
  aws/azure already did).
- **Stereo `media_bug_read` heap overflow on the non-resampled path** —
  lineage copy of the audio_fork fix (the direct path is taken for stereo on
  PCMU/PCMA, since the pipe always negotiates `sample_rate=8000`).
- **`start` on a codec-less session dereferenced a NULL `iananame`**
  (`switch_core_session_get_read_impl` zeroes the struct and returns FALSE
  with no codec negotiated; the return was ignored) — now fails the start.
- **Unload with live sessions destroyed the lws context under them** — same
  gate as audio_fork: live-pipe counter, refuse unload while non-zero.
- **Oversized single-frame inbound messages bypassed `MAX_RECV_BUF_SIZE`** —
  lineage copy of the audio_fork fix.
- **Most WS query parameters were interpolated unencoded** (tag, redact,
  alternatives, endpointing, utterance_end_ms, vad_turnoff, model/tier/
  version, diarize_version, lang) — a value containing `&`/`=`/space/`#`
  silently corrupted the query. All values now go through the encoder.
- **`start <lang> stereo` (without `interim`) silently ran mono** — the
  optional tokens were strictly positional; they now parse in any order, and
  `mono`/`final` are accepted as explicit defaults.
- Chore: dead locals/fields/defines (including the `no_audio_detected` /
  `vad_detected` event defines this module never fires), the metadata-send
  C99 VLA (now a heap vector, parity with the audio_fork copy), the vestigial
  `m_bugname` member and commented-out `deinitialize` wait loop in the pipe.
- README: it documented named concurrent transcriptions (`stop [bugname]`);
  the module supports exactly one per channel.

### mod_google_transcribe
- **Stop/hangup ABBA deadlock** — same class as audio_fork (`cb->mutex` held
  across `switch_core_media_bug_remove` vs hangup CLOSE under `bug_rwlock`);
  the locked region now ends before the remove.
- **`start` on a codec-less session dereferenced a NULL `iananame`** — both
  `start_capture` and `start_capture2`; now fails the start.
- **Stereo/`write` captures stalled and dropped audio on one-sided media** —
  with `fill=SWITCH_TRUE`, `media_bug_read` returns no frame unless BOTH
  directions hold a full frame (verified in FS 1.10.12); the speaking side
  buffered ~512KB then dropped, contradicting the `write` capture's
  documented design. Stereo bugs now read with `fill=SWITCH_FALSE` (the
  silent side is 0xFF-filled ≈ digital silence); mono bugs keep `fill=TRUE`
  (with FALSE an empty read buffer would supply endless fill frames and the
  drain loop would never terminate). Note: the callback is read-driven — if
  the caller's RTP stops entirely, frames stop regardless.
- **Unload with live sessions unmapped module code under the gRPC read
  threads** — live-session counter, refuse unload while non-zero.
- **Unchecked `switch_thread_create`** left a readerless connected stream
  (flow-control stall) — init now fails and tears down.
- **Unchecked `cJSON_PrintUnformatted`** (OOM → NULL → `strcmp` crash in
  `responseHandler`) — guarded at both call sites.
- **VAD-init failure silently fell open to immediate streaming** — with
  `START_RECOGNIZING_ON_VAD` set, a failed `switch_vad_init` connected
  immediately; the start now fails.
- **`uuid_google_transcribe2`'s sample-rate was unvalidated** (atol garbage
  → speex output rate 0) — bounded to 4000..48000.
- **`RECOGNIZER_VAD_DEBUG` was documented but never read** — now honored.
- Chore: dead `SFF_CNG` loop test (FS zeroes `frame->flags` on every read);
  duplicated `RECOGNIZER_VAD_VOICE_MS` read; the "Bug is not attached (race)"
  INFO that fired on every normal stop is now DEBUG.

---

## v0.7.2 — 2026-09-28

Closes both items v0.7.1 left open: the deepgram soak gap it documented as
"deferred, not blocking", and the `google_glue.cpp` **[build]** it marked
pending. Both turned out to be hiding untested code, not just missing
confidence.

### tests/soak: the deepgram soak was never connecting

`mod_deepgram_transcribe`'s AudioPipe dials `LCCSCF_USE_SSL` unconditionally,
so `run.sh` now generates a throwaway self-signed cert and serves the mock over
`wss`, trusted client-side via `SSL_CERT_FILE` (no module-side change, no
committed key, no skip-verify flag). The connected surface is now exercised
directly instead of being deferred to another repo's harness:
`CLIENT_ESTABLISHED`, the pre-handshake `finish()` record-and-complete, the
ring-buffer writes, the inbound receive path, and `CloseStream` teardown
exactly as the production reaper drives it.

Root cause of the silent degradation: `websockets`' default
`select_subprotocol` **raises** `NegotiationError` (HTTP 400) when the server
is configured with a subprotocol list and the client offers none — and the
deepgram pipe offers none (`i.protocol` is commented out in its
`connect_client`). So all 200 iterations failed to connect while the run still
reported `200/200 reapers completed` and passed. The mock now takes a callable
that accepts whatever is offered or falls back to none.

Two related corrections:

- The mock must close the socket when it sees `CloseStream`. The production
  reaper is `finish()` + `waitForClose()` with **no** `close()` of its own — it
  depends on the remote close to fulfill the promise. Without that, every
  graceful teardown in the soak blocks forever.
- `soak_deepgram.cpp` now **fails** unless a majority of iterations reach
  `CONNECT_SUCCESS` (160/200 observed). "Sanitizers clean" and "the path ran"
  are different claims; the counter is what separates them. This assertion is
  what caught the regression in the first place — the first run after the TLS
  change failed with `0 CONNECT_SUCCESS`.

### tests/soak: the oversized-discard path had never executed

Worse than unverified: the `>650KB MAX_RECV_BUF_SIZE` discard path — added in
v0.6.x and cited in v0.7.0 as "regression coverage for the c31364f class" — had
**never run**, in either lineage. `MAX_RECV_BUF_SIZE` is only checked in the
realloc branch, and a single-frame message is allocated whole on its first
fragment (`len + lws_remaining_packet_payload`), so it never reallocs. Growing
in 8KB steps also needs ~82 continuation frames to cross 650KB, and the mock's
per-connection frame counter fired at frame 25 of connections lasting tens of
milliseconds.

The mock now sends a genuinely fragmented oversized message (200×4KB), on the
**first** frame rather than only periodically, and `i % 5 == 1` holds the
connection ~250ms so it can land. Observed on a 50-iteration probe: 68
(audio_fork) and 82 (deepgram) discard events and ~6.7k reassembly reallocs,
ASan clean — versus 0 and 0 before it, where the path had never run at all.

`SOAK_LLL` widens the harness log mask, because the two lineages log the same
recv markers at different severities (audio_fork `lwsl_notice`, deepgram
`lwsl_err`); with the default `LLL_ERR` filter audio_fork's recv path looks
unexercised even when it is running.

### mod_google_transcribe: build gate discharged

Both translation units now compile against the real generated vendor protos
plus real FreeSWITCH 1.10.12 headers — `tests/buildcheck/gen_protos.sh`
generates the googleapis protos (transitive imports included; the generated
`cloud_speech.pb.h` pulls in `google/api/annotations.pb.h` and friends) at a
**pinned** googleapis SHA, since tracking HEAD would make a clean compile
today and a broken one tomorrow with no change here to blame. Link and load
remain a fleet step.

Two warnings that check surfaced, both in code the maintained side had just
touched, now fixed: a `-Wformat-extra-args` (a `switch_log_printf` passing
`var` to a format string with no `%` specifier, so the diarization variable was
never actually logged — inherited from upstream `2565d15`) and a dead
`size_t written` in the resampler block the `write` capture rewrote. The
remaining inherited warnings are baselined in `tests/buildcheck/README.md` so a
new one is visible rather than lost in noise.

### docs

- `AGENTS.md`: what is maintained, the verification legend and how to actually
  run each gate, release discipline, and the harness traps above. There is no
  CI here, so the gates are written down rather than enforced.
- `docs/TESTING.md`: the subprotocol gotcha documented the **wrong** fix
  ("the mock must negotiate `audio.drachtio.org`") — that instruction is what
  produces the 400 for a client offering none. Corrected, plus the new gotchas.
- `tests/soak/README.md` rewritten: the deepgram variant no longer "terminates
  via CONNECT_FAIL", and its receive path is no longer borrowed from
  `mod_ttsd_transcribe`.

### Verification

- **[unit]** `make -C tests` 9/9; `make -C tests sanitize` ASan/UBSan clean;
  `HOST_COVERAGE=100.0%`.
- **[tsan]** `tests/soak` full matrix, **two consecutive** OVERALL PASS runs:
  ASan/UBSan/LSan + TSan for both pipes, 200 iters × 4 workers, 160/200
  `CONNECT_SUCCESS`, 200/200 reapers, no sanitizer findings.
- **[build]** google: *compile* only, both TUs, gcc 14 / debian trixie. Not
  linked or loaded — that stays a fleet step.
- Still unverified: the aws / azure vendor SDK paths (no SDKs on this host) and
  all FreeSWITCH glue.

## v0.7.1 — 2026-09-28

### mod_google_transcribe: a `write` capture

`uuid_google_transcribe <uuid> start <lang> [interim] write [bug-name]`
transcribes only the audio the channel sends, as a mono stream. It captures
both directions (read-driven, so frames keep arriving while nothing is sent)
and forwards the second channel alone.

Why: on the callBroadcast fleet, Google's per-channel recognition of a
`stereo` stream returned results for channel 2 only — channel 1's speech never
came back with the default, `latest_long` or `phone_call` model — although
recognizing the same stream jointly transcribed channel 1, and both channels
carried full-level audio (the debug level logging). Running a `mono` and a
`write` capture under different bug names gives one transcript per direction.

**[build]** and live on FreeSWITCH 1.10.12 (east-1/east-2/west-1, from the
v0.6.3-based commit e191a05). For the rebase onto v0.7.0:
`mod_google_transcribe.c` is **[build]**-clean against the 1.10.12 headers
(the `write_only` plumbing, the `write` argv arm, and the `cap_cb` field set
before bug-add, so no frame can reach the callback unset); `google_glue.cpp`
remains **[build]**-pending — the review host has no gRPC/googleapis
toolchain. Build and load before rolling this tag to a fleet.

### review follow-ups (post-v0.7.0 nits)

- **`MEDIA_BUG_FRAME_BUF_SIZE`** — the five maintained glues sized their
  media-bug frame buffers with a bare `2 * SWITCH_RECOMMENDED_BUFFER_SIZE` and
  a comment pointing at "the azure commit" for the rationale. Promoted to a
  named macro in each module header, rationale stated once in
  `mod_azure_transcribe.h` with pointers from the other four. The 2x factor is
  a proven bound, not a heuristic: `switch_core_media_bug_read()` ends its
  `SMBF_STEREO` path with `memcpy(frame->data, bug->tmp, bytes * 2)` for
  `bytes = read_impl.decoded_bytes_per_packet`, while its `buflen` guard only
  checks the MONO count — and `switch_core_codec_add_implementation()` rejects
  any codec exceeding `SWITCH_RECOMMENDED_BUFFER_SIZE` at registration, so the
  write can never exceed 2x. No behavior change; a `static_assert` /
  `_Static_assert` drift guard sits in each header next to the macro, so every
  real module build enforces it (verified: flipping the factor to 1 breaks the
  build in c11, c++11 and c++17).
  The 16KB stack buffers themselves are unchanged — they are the workaround,
  not the defect; the underlying bug is in FS core.
- **`google_speech_frame` indentation** — `e9d1576` left the `if (streamer &&
  ...)` gate in a third indent style within its block. Cosmetic.

### Verification

- **[unit]** `make -C tests` 9/9; `make -C tests sanitize` ASan/UBSan clean;
  `HOST_COVERAGE=100.0%` (base64 65/65, simple_buffer 31/31).
- All five module headers compile against the real FreeSWITCH 1.10.12 headers
  in c11, c++11 and c++17 (15/15), with the bound guard above; the guard was
  checked by negation, not just for a green build.
- `mod_google_transcribe.c` compiles clean against the 1.10.12 headers.
- Not re-run here: `tests/soak` (needs Docker) and the vendor-SDK builds.

### Known gap (deferred, not blocking)

The `mod_deepgram_transcribe` AudioPipe dials TLS unconditionally, so against
the plain-ws mock every connect ends in `CONNECT_FAIL`. The soak therefore
covers connect-adoption, pre-handshake `finish()`, the reaper promise and
`deinitialize()` — but **none** of the v0.7.0 connected-path changes
(ESTABLISHED record-and-complete, inbound recv reassembly and
oversized-discard, `close()`'s CAS-from-CONNECTED, far-end drop). Closing it
means serving TLS from `ws_mock.py` (self-signed cert generated in `run.sh`);
that needs a Docker-capable host to verify, so it is deliberately left out of
this tag rather than shipped unverified. Until then the deepgram
connected-path rests on the live-credentials soak.

## v0.7.0 — 2026-09-28

Second full adversarial review of the maintained surface (all five
maintained modules + tests/docs infra), one commit per issue.

mod_google_transcribe: hardened the v0.6.3-era stereo-channel debug
instrumentation (e2d2c07) — behavior change: it is now opt-in.
- The per-frame stereo peak-level scan and the per-result `channel_tag`
  DEBUG logging are gated on a new `RECOGNIZER_DEBUG_AUDIO_LEVELS` channel
  variable (default off), so there is zero per-frame cost in normal
  operation (`switch_log_check` does not exist in FS 1.10, so a log-level
  gate was not available).
- Report cadence is now ~1 second of audio at any ptime (per-channel sample
  tally vs the read rate) instead of a hard-coded 50-frame count that
  assumed 20ms ptime; the last-frame `samples`/`datalen` in the report are
  labeled as such, and `transcript_len` was renamed `transcript_bytes`
  (it is a UTF-8 byte count).
- Fixed `cb->samples_per_second`: declared in `cap_cb` and passed to
  `google_speech_session_init` since upstream, but never assigned — any
  future use would have read a zeroed field.
- README: documented the new variable, fixed the `1RECOGNIZER_VAD` typo and
  the duplicated `no_audio_detected` event entry.

Full adversarial review of the maintained surface at HEAD (all five
maintained modules + tests/docs infra), one commit per issue:

### mod_audio_fork (+ the deepgram lineage copy, kept aligned)
- **Stereo resample heap overflow** — `out_len = available >> 1` sized speex's
  interleaved output in mono samples; a stereo resample could write 2x the
  remaining ring-buffer space past `m_audio_buffer`. Deepgram's fix (5d6fc36)
  was never ported to the ancestor despite it exposing stereo.
- **`argv[4]` NULL deref** — `start` with the sampling rate omitted crashed
  the FS process; the argc guard required 4 args, the handler strcmp'd the 5th.
- **`tech_pvt->mutex` destroyed while held, with possible lws-thread waiter** —
  UB: crash or a frozen service thread stalling every session. Port of the
  deepgram pool-owned-mutex fix.
- **CONNECT_SUCCESS raced the reaper** on `pAudioPipe` — null/freed-pipe
  metadata send; now fetched under the reaper's lock.
- **`close()` racing a far-end drop called `lws_callback_on_writable` on a
  freed wsi** — the DISCONNECTING store is now a CAS from CONNECTED.
- **`findAndRemovePendingConnect`'s null-wsi sweep orphaned IDLE pipes** —
  reaper blocked in `waitForClose()` forever, pipe + thread leaked (both
  lineage copies).
- Graceful shutdown during the WS handshake never completed (flush frame
  never sent; also a null-`m_vhd` deref on unadopted pipes) — record-and-
  complete-at-ESTABLISHED, matching `close()`'s 6077c8e pattern.
- media-bug-add failure leaked the never-attached AudioPipe; `tech_pvt->id`
  dereferenced before its null guard; `strncpy` into same-sized buffers never
  NUL-terminated; graceful zero-length `lws_write` failure silently ignored.
- Raw lws connect-error string broke the CONNECT_FAIL event JSON (cJSON now,
  both modules); `constructPath` could emit `?&model=` and a duplicate
  `model=` key (deepgram); deepgram header guard was the copy-pasted aws one;
  dead code removed; READMEs document the real command surface.

### mod_aws_transcribe
- **Transcript left set when the session was gone** — the wait predicate held
  forever: 100% CPU busy-spin, and the v0.6.1 bounded shutdown **failed open**
  (`wait_for` returned immediately, so `DisableRequestProcessing` was never
  reached) exactly on dead-network hangs; `session_stop`'s join hung.
- **Concurrent stop used the media bug after the first stop freed it** — an
  API stop racing the hangup CLOSE (second `stop` during the 10s+ join window)
  ended in `switch_core_media_bug_remove` on a dangling pointer.
- Failed load leaked reserved event subclasses until FS restart; unload ran
  `Aws::ShutdownAPI` under live streaming clients (now refused while sessions
  are active); worker-vs-SDK-callback lock-order inversion; shutdown deadline
  restarted on every wake (now armed once at Close); `catch (...)` on the
  worker; dead code and misleading logs; README documented a nonexistent
  command (`aws_transcribe` vs `uuid_aws_transcribe`) plus half the surface.

### mod_azure_transcribe
- **Write failure during the prebuffer drain silenced the recognizer forever**
  — the exact v0.6.0 defect class, missed on the drain path: `m_finished` set,
  `notifyWriteFailure()` never called, no later event could fire.
- **Terminal Canceled/unexpected-stop events carried
  `transcription-session-finished=false`** — nothing terminal ever followed;
  the consumer could never learn the session ended.
- `switch_separate_string` truncated channel variables in place
  (ALTERNATIVE_LANGUAGE_CODES, HINTS) — a restart on the same channel saw only
  the first token.
- Concurrent same-bugname starts orphaned a live recognizer at hangup (CLOSE
  now stops its own `cb`); stereo media-bug read could smash the 8KB stack
  buffer on wideband/long-ptime codecs (**same hardening applied to
  google/aws/deepgram/audio_fork**); unload crashed via reaper/SDK threads
  running unmapped module code (now refused while recognizers exist); VAD was
  initialized mono on stereo captures; the error subclass was fired but never
  reserved; dead `AudioProcessingOptions` creation; dead code; README
  documented a nonexistent command and much else wrong.

### tests / docs / infra
- The audio_fork soak could not fail on a reaper leak — ported deepgram's
  100% reaper-completion gate (a `waitForClose` hang now exits non-zero).
- The mock never exercised the oversized/fragmented inbound paths — added
  `WS_OVERSIZED_EVERY` (fragmented and >650KB messages), enabled in `run.sh`;
  also fixed `run.sh` double-starting the mocks after that change.
- Versioned the coverage-artifact gitignore entries; removed a
  self-referential symlink in mod_google_tts; soak README scenario names now
  match the code; TESTING.md leads with the autoload load-gate and documents
  the installer's provenance; soak scripts got exec bits and skip the unused
  drop-mock for the TLS-only deepgram runs.

### Verification
- **[unit]** `make -C tests` + `sanitize`: 9/9 pass, ASan/UBSan clean.
- **[tsan]** `tests/soak` (both AudioPipes, Docker): OVERALL PASS — ASan +
  UBSan + LSan and TSan clean, reaper gates 200/200 on both variants, with
  the new fragmented/oversized inbound mode active (inbound events rose from
  ~360 to ~570 per run, confirming the new paths execute).
- **Not verified here:** the Layer-3 Docker build/load gate for the five
  modules (the google/aws/azure glue changes are compile-unverified — no
  FreeSWITCH headers on the authoring host), and live-credential vendor
  streaming (unchanged requirement, see `docs/TESTING.md`).

## v0.6.3 — 2026-07-02
Docs only — a docs-vs-code audit found two shipped claims contradicting the
code after v0.6.1:
- `modules/mod_audio_fork/README.md` still said `MOD_AUDIO_FORK_SERVICE_THREADS`
  "can be set to as many as 5"; values above 1 have been capped to 1 since
  v0.6.1 (multi-context connect adoption is not thread-safe).
- `tests/soak/README.md` still described the soak as `mod_audio_fork`-only; it
  has built and run the `mod_deepgram_transcribe` AudioPipe soak as well since
  v0.6.0 (now documented, including what the TLS-only deepgram variant does and
  does not exercise, and the lws-only `tsan.supp` suppression).

## v0.6.2 — 2026-07-02
Docs only.
- `docs/TESTING.md`: the Layer-3 load gate no longer recommends `fs_cli -x
  "load mod_x"` into a running containerized FS — measured during the v0.6.1
  work, that path intermittently segfaults FS for ANY module (an unmodified
  baseline died on manual load in 4/5 trials) and produces convincing false
  FAILs. Gate via the autoload boot path (modules.conf.xml) instead, with the
  retry-wrapped boot+load sequence as the fallback when manual load is
  unavoidable.

## v0.6.1 — 2026-07-02

Completes the v0.6.0 review: the shared-lineage AudioPipe fixes ported to
`mod_audio_fork`, and both aws deferrals resolved against the actual
aws-sdk-cpp source. **[unit] [build] [tsan]**

- **mod_audio_fork** (5 commits, one per issue — the deepgram v0.6.0 fixes in
  its identical lws transport):
  - `close()` racing the WS handshake leaked the pipe/thread/socket forever
    (reaper blocked in `waitForClose()`); completed via a pending-close flag
    at ESTABLISHED, same pattern as deepgram/ttsd `finish()`.
  - Oversized/alloc-failed inbound messages could deliver a truncated tail to
    the JSON parser as a complete message; unchecked first-fragment malloc;
    realloc-failure leak with poisoned fragment math.
  - `~AudioPipe()` `delete[]`d the malloc'd recv buffer; `lws_write` failures
    hidden by the signed/unsigned comparison.
  - Multi-context set: per-context pending-list filtering, atomic `contexts[]`
    slots, NULL-slot guard, and `MOD_AUDIO_FORK_SERVICE_THREADS` capped at 1.
  - NULL connect-error string streamed into an ostream in the glue.
- **mod_aws_transcribe — both v0.6.0 deferrals closed, verified against the
  aws-sdk-cpp source shipped in the build image:**
  - Shutdown wait is now bounded: if the final outcome is >10s overdue after
    `Close()`, the worker calls `AWSClient::DisableRequestProcessing()`
    (public API; fails the in-flight request at the http-client layer), which
    delivers the error outcome via `OnResponseCallback` and unblocks teardown
    on dead networks.
  - The "delete while the SDK unwinds past OnResponseCallback" concern is
    REFUTED as a bug: `~TranscribeStreamingServiceClient` →
    `ShutdownSdkClient` waits for `m_operationsProcessed == 0` (each async op
    holds an RAIICounter through its full unwind) and then resets the
    executor, whose destructor joins in-flight task threads
    (`AWSClientAsyncCRTP.h`, `DefaultExecutor.cpp`). Documented at the
    delete site.
- The same AudioPipe ports (realloc recovery, allocator mismatch, lws_write,
  multi-context set, bug-add-failure leak) landed in the
  `mod_ttsd_transcribe` repo in the same pass.

### Still open (unchanged from v0.6.0, with verdicts)
- google VAD-path `connect()` on the media thread: **accepted limitation** —
  a safe fix means redesigning the connect signaling (promise set-once and
  prebuffer locking both grow new race surfaces) and warrants its own soak
  cycle; the v0.6.0 keepalive bounds the worst case, and the stall affects
  only that session's own media processing.
- Multi-context lws service threads stay capped at 1 everywhere pending a
  per-context connect-adoption redesign.
- Live-credential vendor streaming remains unverified (needs real creds).

### Verification
- **[unit]** `make -C tests coverage`: 9/9 pass, 100% coverage (unchanged units).
- **[build]** aws + audio_fork rebuilt from the fixed sources and loaded in
  FreeSWITCH 1.10.12 (Docker, audio_fork added to autoload for the test):
  `module_exists` = true for all five maintained modules.
- **[tsan]** `tests/soak` (both AudioPipes) with the ported audio_fork
  sources: OVERALL PASS, ASan+UBSan+LSan and TSan clean, deepgram reaper
  200/200. Note: an earlier in-container flakiness investigation showed that
  manually `load`ing modules into a running containerized FS is unreliable
  for ANY module (baseline included) — load verification uses the autoload
  boot path, which is stable.

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
