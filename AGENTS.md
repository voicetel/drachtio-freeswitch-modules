# AGENTS

Instructions for automated agents working in this repository.

## What is actually maintained

This fork maintains **five** modules — the streaming speech-to-text set used by
callBroadcast plus the audio fork:

- `modules/mod_deepgram_transcribe`, `mod_google_transcribe`,
  `mod_aws_transcribe`, `mod_azure_transcribe`, `mod_audio_fork`

Everything else under `modules/` is inherited from upstream and **out of scope**:
do not "fix" it, do not hold a release for it, and do not treat its warnings as
ours. `mod_audio_fork` and `mod_deepgram_transcribe` ship a shared `AudioPipe`
**lineage copy** — a transport fix must be applied to both, and the headers
duplicate `MEDIA_BUG_FRAME_BUF_SIZE` for that reason.

## Verification legend (`CHANGELOG.md`)

| badge | means | how to run |
| --- | --- | --- |
| **[unit]** | host unit tests + ASan/UBSan | `make -C tests` then `make -C tests sanitize` |
| **[build]** | compiles, links and loads in FreeSWITCH 1.10.12 | fleet step; partial offline for google via `tests/buildcheck/` |
| **[tsan]** | ThreadSanitizer-clean | `tests/soak` (Docker, below) |

Never claim a badge you did not run. If a change is unverified, say so in the
changelog with what is pending and why — that is the established pattern here
and it is what makes the changelog trustworthy.

## Commands

```sh
make -C tests                 # host unit tests (9 tests; must be 9/9)
make -C tests sanitize        # same under ASan + UBSan
make -C tests coverage        # parses to a HOST_COVERAGE= line
```

`HOST_COVERAGE` covers the **host-testable units only** — it is not repo-wide
or module coverage, and quoting it as such is misleading.

Concurrency soak (the real `AudioPipe`, both lineages):

```sh
docker build -f tests/soak/Dockerfile -t audiopipe-soak .   # context = repo root
docker run --rm --security-opt seccomp=unconfined audiopipe-soak
```

`seccomp=unconfined` is required (`setarch -R` disables ASLR for TSan). On a
review host the docker daemon is often reachable only via `sudo`. Expect ~4
sanitizer runs; a full pass is several minutes. **Run it twice** — concurrency
harnesses flake, and a single green run is not evidence.

Vendor-SDK compile gate (google only; aws/azure need their SDKs):

```sh
tests/buildcheck/gen_protos.sh "$(mktemp -d)/gens"   # see tests/buildcheck/README.md
```

There is **no CI in this repo.** Every gate above is a manual pre-tag step,
which is exactly why release notes must record what was and was not run.

## Release discipline

- **One commit per issue.** Do not batch unrelated fixes into one commit.
- Conventional prefixes, scoped to the module: `fix(google): …`,
  `chore(azure): …`, `docs(soak): …`, `test(soak): …`, `refactor(modules): …`.
  The subject says *what was wrong*, not what was done: `fix(azure): VAD
  initialized mono on stereo captures` beats `fix(azure): improve VAD`.
- Add a `CHANGELOG.md` entry under an `## Unreleased` heading as you go; fold it
  into the release commit when you tag.
- Tag annotated, body summarising what changed **and what is unverified**:
  `git tag -a v0.x.y -F - <<'MSG' … MSG`
- `git push --follow-tags origin main` pushes branch + tag together.
- **Never move a published tag.** If `v0.7.1` is on the remote, it stays where
  it is; corrections go in `v0.7.2`.
- Before touching a branch that others push to, `git fetch` and check
  `git log HEAD..origin/main` — this repo gets rebased features landing
  concurrently.

## Comments and code

- Comment the **why**, especially where a fix declines to act: "deliberately NOT
  wiring it in now: enabling the SDK's default audio processing on already-clean
  telephony audio is a consumer-visible behavior change that needs a
  live-credential soak first" is the standard to match.
- Record provenance in comments where the reader would otherwise need git
  archaeology: upstream commit ids (`5d6fc36`), the defect class being ported,
  and which upstream FS source proves a bound.
- Do not add bare-what comments or restate the obvious.

## Harness traps (learned the expensive way)

These each silently produced a false green. Full write-ups in
`docs/TESTING.md` § "Soak gotchas" and `tests/soak/README.md`.

- A mock configured with a fixed `subprotocols` list **rejects** a client that
  offers none (`NegotiationError` → HTTP 400). The deepgram pipe offers none, so
  its soak degraded to connect-fail-only while reporting 200/200 and passing.
- "Sanitizers clean" ≠ "the path ran". Assert coverage (`CONNECT_SUCCESS`
  counters, or `SOAK_LLL=7` plus a grep for the module's own log markers) before
  believing a green run.
- The two pipe lineages log the same recv markers at **different severities**
  (audio_fork `lwsl_notice`, deepgram `lwsl_err`).
- `MAX_RECV_BUF_SIZE` is only checked in the realloc branch: an oversized probe
  must be sent *fragmented*, *early*, with the connection *held open*.
- Prove a guard by negation. A `static_assert` that has never failed is not known
  to work — flip the bound and confirm the build breaks.
