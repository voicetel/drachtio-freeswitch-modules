# FreeSWITCH module testing playbook

How the `drachtio-freeswitch-modules` STT/audio modules were tested, written so the
same process can be applied to other modules. Covers four independent layers and,
at the end, a decision tree for a new module.

Reality up front: these modules build **only inside the FreeSWITCH source tree**
(autotools + `libfreeswitch` + vendor SDKs). There is no standalone build. So
"testing" is layered, and each layer catches a different class of problem. Be
honest about which layer proves what:

| Layer | What it proves | What it does NOT prove |
|---|---|---|
| 1. Static analysis | suspicious patterns, some memory/UB bugs | no data races; many false positives without FS headers |
| 2. Host unit tests (+ASan/UBSan +coverage) | pure logic correct, memory-safe, covered | nothing that needs FS runtime or vendor SDKs |
| 3. Docker FS build + load smoke test | compiles, links, `dlopen`s with all symbols resolved | runtime behavior; concurrency |
| 4. Standalone ASan/TSan concurrency soak | no data races / UAF / leaks in transport+lifecycle | the FS glue; the vendor-SDK paths |
| (5. live-credential soak) | end-to-end behavior + full concurrency | — (this one needs your creds + calls) |

Layers 1–4 are all doable on a dev box / in Docker. Layer 5 needs real vendor
credentials and live calls — it can't be faithfully mocked for SDK-based vendors
(AWS/Azure) and is the only thing that confirms "zero races" under production load.

---

## Layer 1 — Static analysis (cheap, run first)

cppcheck tolerates missing FS headers; clang-tidy mostly needs them (run it inside
the FS tree — see Layer 3 — with the FS include paths).

```sh
# per module, C++ mode, suppress the unavoidable cross-module / missing-include noise
cppcheck --language=c++ --enable=warning,performance,portability --inline-suppr --quiet \
  --suppress=missingInclude --suppress=missingIncludeSystem --suppress=unknownMacro \
  --suppress=ctuOneDefinitionRuleViolation \
  modules/<mod>/*.c modules/<mod>/*.cpp modules/<mod>/*.h modules/<mod>/*.hpp
```

cppcheck does NOT detect data races — don't rely on it for concurrency. Treat its
output as advisory; triage real findings (unchecked malloc, uninit members, etc.).

---

## Layer 2 — Host unit tests + ASan/UBSan + coverage

Only logic with **no** FreeSWITCH runtime / vendor-SDK / network dependency is
host-testable (e.g. `base64.hpp`, `SimpleBuffer` in `simple_buffer.h`). Do not mock
the FS core to fake coverage — test the genuinely-pure pieces and say plainly what
isn't host-testable.

In this repo: `tests/` (a tiny header-only harness `test_util.h` + `test_*.cpp`,
a `Makefile`).

```sh
make -C tests            # build + run
make -C tests coverage   # + gcov; prints HOST_COVERAGE=<pct>%
make -C tests sanitize   # build + run under AddressSanitizer + UndefinedBehaviorSanitizer
```

The `sanitize` target compiles the same units with `-fsanitize=address,undefined
-fno-omit-frame-pointer` and runs them — real runtime memory-safety/UB checking of
that logic. To add a unit: write `tests/test_<x>.cpp` using `test_util.h`
(`TEST(name){ CHECK(...); CHECK_EQ(a,b); }`), point its `-I` at the module that owns
the header, and add it to `TESTS`/`COV_SRCS` in `tests/Makefile`.

---

## Layer 3 — Docker FS build + load smoke test (authoritative compile/link/load)

This proves the module compiles, links, and `dlopen`s into a running FreeSWITCH
with all `switch_*` symbols resolved (and its API verb registers). It reuses
callBroadcast's known-good FreeSWITCH installer with the module branch injected.

### Pattern
1. Build context in scratch (NOT the repo): copy `callBroadcast/freeswitch/`, and
   `git clone --branch <branch> <repo>` into it.
2. Dockerfile: `FROM debian:trixie`, install build deps, run the installer with the
   branch wired in, then a smoke test.
3. Inject the branch via the installer's env knobs:
   `ENV DRACHTIO_REPO=/src/drachtio-src DRACHTIO_REF=<branch>`.

### Gotchas that each cost a failed build (bake these in)
- **`libpcre2-dev`** — FS 1.10.12 `configure` hard-requires `libpcre2-8.pc`, which
  the installer only gets transitively. Add `libpcre2-dev` to the Dockerfile apt
  line or configure aborts. (This is a latent fragility in callBroadcast's
  installer too — it should add it to its apt list.)
- **`JOBS=2`** — the installer uses `make -j$(nproc)`; on a 14 GiB host with other
  processes, `-j20` (and even `-j4`) OOM-kills the FreeSWITCH link (exit 137). Set
  `ENV JOBS=2` (honored by the installer). Prune stopped containers/dangling images
  first to free space/RAM.
- **Private modules** — `mod_audio_stream` / `mod_siprec` come from private repos
  needing `DEPLOY_REPO_PAT`. In the context copy of the installer, neuter
  `install_mod_audio_stream` to a no-op and soften the `mod_audio_stream.so` `die`
  in `verify()` to a `warn`.
- **`mod_audio_fork` is not built by the installer** (callBroadcast dropped it). Add
  a Dockerfile `RUN` that compiles it from the staged source with the same recipe
  the installer uses for the libwebsockets-based modules (gcc/g++ `-c` each source
  with `-I$FS_SRC/src/include -I$FS_SRC/libs/libteletone/src -I/usr/include/uuid` +
  FS cflags, then `g++ -shared ... $(pkg-config --libs libwebsockets) -lstdc++
  -lpthread`, install to `$PREFIX/lib/freeswitch/mod/`).

Key installer paths inside the image: `PREFIX=/usr/local/freeswitch`,
`FS_SRC=/usr/local/src/callbroadcast/freeswitch`,
`FORK_SRC=/usr/local/src/callbroadcast/drachtio-freeswitch-modules`.

### The smoke test (the load gate) — do NOT parse `load` output
`fs_cli -x "load mod_x"` prints `+OK Reloading XML` *before* the real result, which
masks `-ERR module load file routine returned an error`. Use `module_exists`
(authoritative): a module whose `_load` returned error will not exist.

```sh
/usr/local/freeswitch/bin/freeswitch -nonat -nc -nonatmap -nf >/tmp/fs.log 2>&1 &
sleep 12
for m in <modules that autoload>; do
  r=$(fs_cli -x "module_exists $m" | tr -d '[:space:]')
  [ "$r" = true ] && echo "OK: $m" || { echo "FAIL: $m=$r"; tail -150 /tmp/fs.log; exit 1; }
done
```

### Non-autoloaded modules: put them ON the autoload path for the gate
Do NOT gate on `fs_cli -x "load mod_x"` into an already-running containerized
FS. Measured during the v0.6.1 work: that path intermittently segfaults FS for
ANY module — an unmodified baseline build died on manual load in 4 of 5 trials
in `cb-fs-install-test`, while the same `.so` loaded fine every time on the
autoload boot path (and under gdb's slowed startup). It is an in-container FS
instability, not a module signal, and it produces convincing-looking false
FAILs (and can mask real ones behind retries).

For the gate, add the module to `autoload_configs/modules.conf.xml` in the
container and assert `module_exists` after boot:

```sh
CONF=/usr/local/freeswitch/etc/freeswitch/autoload_configs/modules.conf.xml
grep -q mod_audio_fork $CONF || \
  sed -i 's|<load module="mod_aws_transcribe"/>|<load module="mod_aws_transcribe"/>\n    <load module="mod_audio_fork"/>|' $CONF
# ...boot FS as above, then:
[ "$(fs_cli -x 'module_exists mod_audio_fork'|tr -d '[:space:]')" = true ] && \
[ "$(fs_cli -x 'show api'|grep -c '^uuid_audio_fork,')" -ge 1 ] && echo OK || { echo FAIL; exit 1; }
```

If a manual `load` is unavoidable, wrap the WHOLE boot+load sequence in a
retry loop (the mod_ttsd_transcribe `tests/loadtest` `bring_up()` pattern) and
treat only repeated failures as a signal.

Build is ~15–30 min at `JOBS=2`; the image is ~3.5 GB — delete it and the scratch
context afterward. This catches real bugs: it caught a duplicate `m_gracefulShutdown`
initializer and a fatal double-reservation of a shared event subclass
(`jambonz_transcribe::error`) that made the 2nd/3rd module's `_load` fail.

---

## Layer 4 — Standalone ASan/TSan concurrency soak (the high-value one)

This is the only way to get real **ThreadSanitizer** results here. The trick that
makes it possible: the WebSocket transport class (`AudioPipe` in mod_audio_fork /
the deepgram lineage) depends only on **libwebsockets + the C++ stdlib — zero
`switch_*` symbols**. So it can be compiled and driven *without FreeSWITCH*, which
means you control the whole binary and TSan works.

Check first whether the concurrency-bearing code is FS-free:
```sh
grep -oE 'switch_[a-z_]+' modules/<mod>/<file>.cpp | sort -u   # empty -> harness-able standalone
```

The harness (`tests/soak/`) builds the **real** module source twice — once with
`-fsanitize=address,undefined` and once with `-fsanitize=thread` — for BOTH
lws AudioPipes (`mod_audio_fork` and, since v0.6.0, `mod_deepgram_transcribe`;
deepgram dials TLS-only, so its soak covers the connect-fail/teardown/reaper
surfaces plus a 200/200 reaper-completion gate, while its receive path is
byte-identical to the mod_ttsd_transcribe AudioPipe soaked end-to-end in that
repo) — and drives, concurrently in a loop:
- the real lws service thread (started by `AudioPipe::initialize`),
- a "media thread" that writes audio into the ring buffer exactly like the media-bug
  callback does,
- the teardown/reaper (`close` -> `waitForClose` -> delete via `shared_ptr`),
against a mock WebSocket server, cycling the scenario matrix:
**graceful stop · hangup-mid-stream · far-end drop · rapid restart · connect-fail.**

```sh
# build context = repo root so it uses the LIVE module source
docker build -f tests/soak/Dockerfile -t audiopipe-soak .
docker run --rm --security-opt seccomp=unconfined audiopipe-soak     # OVERALL: PASS == both sanitizers clean
```

### Soak gotchas
- **TSan "unexpected memory mapping"** on recent kernels (ASLR entropy too high):
  run the binary under `setarch -R` (disables ASLR). Needs `util-linux` +
  `--security-opt seccomp=unconfined` so the `personality()` syscall is allowed.
- **TSan can't be `LD_PRELOAD`ed** (unlike ASan) — the binary must be *built* with
  `-fsanitize=thread`. That's why the standalone harness exists: instrumenting all
  of FreeSWITCH with TSan is impractical, but the FS-free transport class is easy.
- **Mirror the real call ordering in the harness or you'll chase phantom bugs.**
  `initialize()` spawns the service thread that creates the lws context
  *asynchronously*; in FreeSWITCH the first `connect()` is seconds later, so the
  harness must `sleep` after `initialize()` before connecting (otherwise
  `connect()` dereferences a NULL `contexts[]` — a harness bug, not a module bug).
- **WebSocket subprotocol** — the libwebsockets client offers a subprotocol
  (`audio.drachtio.org`); the mock server (python `websockets`) must negotiate it:
  `serve(..., subprotocols=["audio.drachtio.org"])`, or the handshake fails.
- **Classify every sanitizer finding** as a real module bug vs a harness/shutdown
  artifact before acting. This soak's TSan run found a genuine race — but only in
  the module-unload `deinitialize()` path (detached threads + an unlocked
  `std::map` read + destroy-without-join), not the per-call lifecycle. That was a
  real bug worth fixing; a naive reading would have mislabeled it.

This layer found and we fixed: the `deinitialize()` shutdown race; and it
*confirmed* the earlier per-call fixes (the AudioPipe use-after-free rework and the
cross-thread atomics) hold up — ASan + TSan clean across 200 iters × 4 workers.

---

## Layer 5 — Live-credential soak (yours to run)

Real ASan/TSan of the modules' *full* concurrency (media thread × vendor
SDK/gRPC/ws threads × teardown) needs a real backend, because the vendor threads
only fire when actually connected:
- **ws-based (mod_audio_fork, deepgram)** can use a mock ws server (Layer 4 already
  does the transport; full glue needs FS + a ws sink).
- **google** speaks gRPC — a mock `StreamingRecognize` server is buildable from the
  public proto (significant effort).
- **aws / azure** use the AWS SDK (SigV4 + HTTP/2 + `vnd.amazon.eventstream`) and
  the MS Speech SDK (proprietary USP-over-wss). These validate TLS/auth/framing
  against their real backends; a faithful in-container mock is not realistically
  achievable. Soak them against the vendor sandbox with test credentials.

To build a module with sanitizers inside the FS tree (for Layer 5), append to the
per-module compile/link in the installer recipe:
`-fsanitize=address,undefined -fno-omit-frame-pointer -g` (ASan; LD_PRELOAD
`$(gcc -print-file-name=libasan.so)` into stock FS), or, separately,
`-fsanitize=thread` (needs FS itself built with TSan to be low-noise). See
`tests/SANITIZERS.md` for the run setup and the scenario list.

---

## Applying this to a NEW module — decision tree

1. **Static (Layer 1)** — always. cppcheck per module; clang-tidy inside the FS
   tree if you want analyzer/concurrency checks.
2. **Pure logic? (Layer 2)** — is there logic with no FS/SDK/network dependency
   (parsers, buffers, codecs, framing math)? Add host unit tests + run them under
   `make sanitize`; report coverage. If nothing qualifies, say so.
3. **Compile/link/load (Layer 3)** — always, before merge. Add the module to the
   Docker FS build + `module_exists` smoke test. If it's not built by the installer,
   add a manual compile `RUN` mirroring a sibling with the same deps.
4. **Concurrency code? (Layer 4)** — does it have its own threads / shared state
   (a transport class, a worker thread, SDK callbacks)? Run
   `grep -oE 'switch_[a-z_]+' <file>` — if it's FS-free, build a standalone
   ASan+TSan harness (copy `tests/soak/` and adapt the driver to that class's API +
   a matching mock server). If it's NOT FS-free, you're limited to ASan-via-
   LD_PRELOAD inside FS (Layer 5) — TSan needs the whole-process build.
5. **End-to-end (Layer 5)** — for production confidence on vendor-SDK paths, soak
   against the real vendor sandbox with test creds under ASan (and TSan if you can
   build FS instrumented). This is the only layer that proves "zero races under
   load"; everything above is necessary but not sufficient for that claim.

### Honesty checklist for any test report
- Distinguish **compiled/linked/loaded** from **runtime-verified**.
- "ASan/TSan clean" means *for the scenarios actually exercised* — state them.
- Name what was NOT covered (glue, vendor SDKs, scenarios not run).
- A green Docker `module_exists` is load-time only; it is not a behavior test.
