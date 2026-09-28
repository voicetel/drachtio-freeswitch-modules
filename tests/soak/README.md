# AudioPipe concurrency soak (ASan + TSan)

A Dockerized concurrency soak of the **real** lws `AudioPipe` transports —
since v0.6.0 it builds and runs BOTH the `mod_audio_fork` pipe and the
`mod_deepgram_transcribe` pipe (`tests/soak/soak_deepgram.cpp`). The lifecycle
under test (connect / stream / far-end drop / graceful close / rapid restart /
connect-fail / reaper teardown / shutdown) is the most concurrency-sensitive
code in the maintained modules.

The deepgram variant dials TLS unconditionally (`LCCSCF_USE_SSL`, with no CA
path and no skip-verify flag), so `run.sh` generates a throwaway self-signed
certificate and serves the mock over `wss`, which the client trusts through
`SSL_CERT_FILE` — no module-side change and no committed key. The handshakes
therefore **complete**, and the connected surface is exercised directly:
`CLIENT_ESTABLISHED` (including the pre-handshake `finish()`
record-and-complete), the ring-buffer writes, the inbound receive path, and
`CloseStream` teardown exactly as the production reaper drives it (`finish()`
then `waitForClose()`, never `close()` — it relies on the *remote* close, which
the mock models by closing when it sees `CloseStream`).

`soak_deepgram.cpp` fails unless a majority of its iterations reach
`CONNECT_SUCCESS`. Before v0.7.2 the harness had no such assertion, so when the
mock rejected the upgrade (websockets' default `select_subprotocol` raises
`NegotiationError` against a client that offers no subprotocol, which the
deepgram pipe does) the run still reported 200/200 reapers and passed while
proving nothing about the connected path. The counter exists to make that
failure mode loud.

lws-internal logging races are suppressed via `tsan.supp` (library-only frames;
module frames are never suppressed).

`AudioPipe` depends only on libwebsockets + the C++ stdlib (no FreeSWITCH), so it
can be driven directly: the real lws service thread, a media-thread writer that
fills the ring buffer exactly like `fork_frame`, and the teardown/reaper
(`close` → `waitForClose` → delete via `shared_ptr`, capturing by value) all run
concurrently in a 200-iteration × 4-worker loop against a mock ws server — built
**twice from the same real source**, once with ASan/UBSan/LeakSanitizer and once
with ThreadSanitizer.

## Run

```sh
# from the repo root (build context must be the repo root):
docker build -f tests/soak/Dockerfile -t audiopipe-soak .
docker run --rm --security-opt seccomp=unconfined audiopipe-soak
```

`seccomp=unconfined` is needed so `setarch -R` (disables ASLR — required for TSan
on recent kernels) and the sanitizer runtimes work. Exit 0 / `OVERALL: PASS`
means ASan and TSan were both clean.

## What it does and doesn't cover

- **Covers:** data races, use-after-free, double-free, heap overflow, and leaks
  in the `AudioPipe` transport + lifecycle + the shutdown (`deinitialize`) path,
  for both pipes and on both the connected and the connect-fail paths.
- **Does not cover:** the FreeSWITCH glue (`lws_glue.cpp`: media-bug lifecycle,
  `eventCallback`, `fork_session_cleanup`) which needs the FS runtime, and the
  google/aws/azure vendor SDK paths (gRPC / SigV4-HTTP2 / MS Speech SDK) which
  need real backends — see `../SANITIZERS.md`. Those remain a real-credentials
  live soak.

## Scenarios (per iteration, round-robin, `i % 5`)

graceful stop (zero-length flush frame, audio_fork) · plain close via reaper ·
far-end drop (mock server closes mid-stream) · text send + immediate
teardown · connect-fail (dead port). `i % 5 == 1` holds the connection open
No iteration
restarts a pipe — "rapid restart" coverage comes from the 200×4 rapid
connect/teardown cadence itself. Both variants gate on 100% reaper
completion (a `waitForClose` hang fails the run), and the deepgram variant
additionally gates on `CONNECT_SUCCESS`.

The mock server pushes occasional inbound JSON, plus — when
`WS_OVERSIZED_EVERY` is active (run.sh default) — deliberately fragmented and
>650KB oversized messages for the recv reassembly and discard paths.
