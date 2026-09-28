# Build check (compile gate for `mod_google_transcribe`)

Discharges the **[build]** half of the verification legend for
`mod_google_transcribe` on a machine that has neither a FreeSWITCH build tree
nor the googleapis generated protos — which is the common case for a review
host, and exactly why the `google_glue.cpp` changes sat marked
**[build]-pending** through v0.7.1.

**What this proves:** both translation units compile, with the real generated
vendor protos and the real FS headers. **What it does not prove:** linking
against `libfreeswitch.la` and loading in a running FreeSWITCH 1.10.12 — still
a fleet step (`docs/TESTING.md`).

## Prerequisites

```sh
apt-get install -y g++ git protobuf-compiler protobuf-compiler-grpc \
  libprotobuf-dev libgrpc++-dev libabsl-dev libspeexdsp-dev
```

FreeSWITCH headers: `switch.h` pulls in configure-generated headers
(`switch_am_config.h`, `switch_cJSON.h`, …) that are **not** in the vanilla git
tree, so point `FSINC` at a *configured* build tree (`$FS/src/include` plus the
generated headers) or at an installed `freeswitch-dev` header dir.

## Run

```sh
GENS=$(mktemp -d)/gens
tests/buildcheck/gen_protos.sh "$GENS"

g++ -std=c++17 -Wall -Wextra -c -I"$GENS" -I"$FSINC" \
    -Imodules/mod_google_transcribe \
    modules/mod_google_transcribe/google_glue.cpp -o /tmp/google_glue.o
gcc   -std=gnu99 -Wall -Wextra -c -I"$FSINC" \
    -Imodules/mod_google_transcribe \
    modules/mod_google_transcribe/mod_google_transcribe.c -o /tmp/mod_google_transcribe.o
```

`gen_protos.sh` takes an optional second argument — a googleapis checkout you
already have — to avoid re-cloning on every run.

## Warning baseline

`-Wall -Wextra` is not clean on this module and is not made so here: the
remaining warnings are inherited from upstream and out of the maintained
surface's scope. Recorded so a *new* warning is visible rather than lost in
noise (v0.7.2, gcc 14 / debian trixie):

| site | warning |
| --- | --- |
| `google_glue.cpp:100` | unused parameter `samples_per_second` |
| `google_glue.cpp:227` | unused variable `context` |
| `google_glue.cpp:490` | unused parameter `thread` |
| `google_glue.cpp:580` | deprecated `WordInfo::speaker_tag()` |
| `mod_google_transcribe.c:154` | unused params `session`, `data`, `len` |

Two warnings this check *did* clear, both in code the maintained surface had
just touched: a `-Wformat-extra-args` (a `switch_log_printf` passing `var` to
a format string with no `%` specifier — the value was never logged) and a dead
`size_t written` in the resampler block the `write` capture rewrote.
