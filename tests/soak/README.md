# AudioPipe concurrency soak (ASan + TSan)

A Dockerized concurrency soak of the **real** `mod_audio_fork` `AudioPipe` —
the WebSocket transport whose lifecycle (connect / stream / far-end drop /
graceful close / rapid restart / connect-fail / reaper teardown / shutdown) is
the most concurrency-sensitive code in the maintained modules.

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
  in the `AudioPipe` transport + lifecycle + the shutdown (`deinitialize`) path.
- **Does not cover:** the FreeSWITCH glue (`lws_glue.cpp`: media-bug lifecycle,
  `eventCallback`, `fork_session_cleanup`) which needs the FS runtime, and the
  google/aws/azure vendor SDK paths (gRPC / SigV4-HTTP2 / MS Speech SDK) which
  need real backends — see `../SANITIZERS.md`. Those remain a real-credentials
  live soak.

## Scenarios (per iteration, round-robin)

graceful stop · hangup-mid-stream · far-end drop (mock server closes mid-stream) ·
rapid restart · connect-fail (dead port). The mock server also pushes occasional
inbound JSON to exercise the receive path.
