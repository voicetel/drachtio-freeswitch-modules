# Host unit tests

Tests for the FreeSWITCH-**independent** logic in the modules — the only code
that can be compiled and exercised without the FreeSWITCH source tree and the
vendor SDKs (libwebsockets, gRPC, AWS, Azure).

Currently covered:

- `base64.hpp` — `drachtio::base64_encode` / `base64_decode` (RFC 4648 vectors,
  full-byte round-trip, buffer/string overloads).
- `simple_buffer.h` — `SimpleBuffer` FIFO (partial-fill read order, empty-buffer
  underflow guard, multi-chunk add, non-multiple rejection, full-buffer overwrite
  order). These pin the behavior of the fixed circular buffer.

```sh
make            # build + run
make coverage   # build + run + gcov; prints HOST_COVERAGE=<pct>%
make clean
```

`HOST_COVERAGE` is the line coverage of the host-testable units **only**. It is
not module coverage and not repo-wide coverage — the modules themselves build
only inside the FreeSWITCH tree and are not measured here.

Module *glue* (`*_glue.cpp`, `mod_*.c`) is **not** host-testable: it depends on
`switch_core_*`, media bugs, and live vendor streams. Do not mock the FreeSWITCH
core to fake coverage — test the pure logic, and verify glue behavior in a real
FreeSWITCH build.
