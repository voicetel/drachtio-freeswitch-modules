# drachtio-freeswitch-modules (VoiceTel fork)

A fork of [drachtio/drachtio-freeswitch-modules](https://github.com/drachtio/drachtio-freeswitch-modules).

**VoiceTel actively maintains only a small subset of this repository** — the
streaming speech-to-text modules and `mod_audio_fork`, which we ship in
production. Everything else is inherited from upstream and kept **as-is,
unmaintained**.

## Maintained modules

These are the only modules VoiceTel patches, builds, and verifies.

**Streaming speech-to-text** — each attaches a media bug to a channel and streams
L16 audio to the vendor, returning transcripts as FreeSWITCH custom events:

| Module | Transport | Vendor |
|---|---|---|
| [`mod_deepgram_transcribe`](modules/mod_deepgram_transcribe/README.md) | libwebsockets | Deepgram |
| [`mod_google_transcribe`](modules/mod_google_transcribe/README.md) | gRPC | Google Cloud Speech-to-Text |
| [`mod_aws_transcribe`](modules/mod_aws_transcribe/README.md) | HTTP/2 event-stream | AWS Transcribe streaming |
| [`mod_azure_transcribe`](modules/mod_azure_transcribe/README.md) | push-stream | Microsoft Cognitive Services Speech |

**Audio streaming** — [`mod_audio_fork`](modules/mod_audio_fork/README.md):
streams L16 audio over WebSockets (libwebsockets) to your own server, with
optional playback injection back into the call. Recently hardened (memory
safety, thread-safety, input validation), verified to build and load in
FreeSWITCH 1.10.12. **Caveat:** a remaining AudioPipe lifecycle / use-after-free
rework is still pending, so treat it as actively-being-hardened rather than
finished.

## Everything else

All other modules in `modules/` (`mod_google_tts`, `mod_dialogflow`,
`mod_aws_lex`, and the rest) are inherited from upstream and are **not maintained
here**. They are left untouched for reference; we do not build, test, or fix them.

## Building

These are FreeSWITCH loadable modules — they build **inside the FreeSWITCH
source tree**, not standalone. There is no top-level build in this repo.

The maintained modules are verified to compile, link, and load in
**FreeSWITCH 1.10.12**. Each needs its library/SDK present at build time:

- `mod_audio_fork` — libwebsockets
- `mod_deepgram_transcribe` — libwebsockets
- `mod_google_transcribe` — gRPC + the Google Cloud Speech protobuf stubs
- `mod_aws_transcribe` — AWS SDK for C++ (`transcribestreaming`)
- `mod_azure_transcribe` — Microsoft Cognitive Services Speech SDK

At runtime each module reads its credentials from channel variables or
environment (see the module's own README).

## Tests

`tests/` contains host-side unit tests for the FreeSWITCH-independent logic that
can be exercised without the full FreeSWITCH tree or the vendor SDKs:

```sh
make -C tests          # build + run
make -C tests coverage # + gcov line coverage
```

Module glue that depends on the FreeSWITCH runtime and live vendor streams is
not host-testable and is verified by building/loading inside FreeSWITCH.

## Contributing

Patches to the **maintained modules** are accepted via the
[voicetel/drachtio-freeswitch-modules](https://github.com/voicetel/drachtio-freeswitch-modules)
tracker. Issues or PRs against the unmaintained modules, or against upstream
drachtio, will not be actioned here.

## License

See [LICENSE](LICENSE).
