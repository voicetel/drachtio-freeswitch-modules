# mod_aws_transcribe

A Freeswitch module that generates real-time transcriptions on a Freeswitch channel by using AWS streaming transcription API

## API

### Commands
The freeswitch module exposes the following API commands:

```
uuid_aws_transcribe <uuid> start <lang-code> [interim] [stereo|mono] [bugname]
```
Attaches media bug to channel and performs streaming recognize request.
- `uuid` - unique identifier of Freeswitch channel
- `lang-code` - a valid AWS [language code](https://docs.aws.amazon.com/transcribe/latest/dg/streaming.html) that is supported for streaming transcription
- `interim` - If the 'interim' keyword is present then both interim and final transcription results will be returned; otherwise only final transcriptions will be returned
- `stereo` - If the 'stereo' keyword is present, both caller and callee audio are captured as a two-channel (stereo) stream; otherwise only the caller's audio is captured. **Stereo requires `AWS_ENABLE_CHANNEL_IDENTIFICATION` to be set** — AWS rejects `NumberOfChannels=2` without it (BadRequestException) and the module now warns at start time.
- `bugname` - optional name for the media bug (default: `aws_transcribe`)

```
uuid_aws_transcribe <uuid> stop [bugname]
```
Stop transcription on the channel.

### Limits
- AWS closes a streaming transcription after **4 hours**; the session ends with a `jambonz_transcribe::error` event. Restart the transcription on longer calls.
- On `stop`, up to ~10s of already-buffered audio is flushed to AWS before the stream closes (bounded by the 10s final-response deadline); on a dead network the pending request is aborted 10s after close so teardown cannot hang.

### Authentication
The plugin will first look for channel variables, then environment variables.  If neither are found, the AWS SDK's default credential provider chain is used.

The names of the channel variables and environment variables are:

| variable | Description |
| --- | ----------- |
| AWS_ACCESS_KEY_ID | The Aws access key ID |
| AWS_SECRET_ACCESS_KEY | The Aws secret access key |
| AWS_REGION | The Aws region |

### Command Variables
Additional options can be set through freeswitch channel variables:

| variable | Description |
| --- | ----------- |
| START_RECOGNIZING_ON_VAD | if set to 1 or true, do not begin streaming audio to AWS until voice activity is detected |
| RECOGNIZER_VAD_MODE | An integer value 0-3 from less to more aggressive vad detection (default: 2) |
| RECOGNIZER_VAD_SILENCE_MS | Milliseconds of silence before the VAD resets (default: 150) |
| RECOGNIZER_VAD_VOICE_MS | Milliseconds of voice activity required to trigger the connection to AWS when START_RECOGNIZING_ON_VAD is set (default: 250) |
| RECOGNIZER_VAD_DEBUG | if >0 vad debug logs will be generated (default: 0) |
| AWS_SHOW_SPEAKER_LABEL | enable speaker diarization |
| AWS_ENABLE_CHANNEL_IDENTIFICATION | enable channel identification (with stereo capture) |
| AWS_VOCABULARY_NAME | custom vocabulary to use |
| AWS_VOCABULARY_FILTER_NAME | vocabulary filter to use |
| AWS_VOCABULARY_FILTER_METHOD | vocabulary filter method (`mask` or `remove`) |
| AWS_TRANSCRIBE_MAX_BUFFERED_FRAMES | environment variable (not a channel variable): cap on buffered 20ms frames before drop-oldest kicks in under back-pressure |

### Events
`aws_transcribe::transcription` - returns an interim or final transcription.  The event contains a JSON body describing the transcription result:
```js
[
  {
    "is_final": true,
    "alternatives": [{
      "transcript": "Hello. Can you hear me?"
    }]
  }
]
```

All events carry a `transcription-vendor: aws` header; `aws_transcribe::vad_detected` is fired when speech is detected on the VAD path; `jambonz_transcribe::error` is fired on stream errors (the subclass name is inherited and frozen -- external consumers key on it).

The module reserves `aws_transcribe::end_of_transcript`, `aws_transcribe::no_audio_detected` and `aws_transcribe::max_duration_exceeded` for compatibility, but **it never fires them** (no producer exists); do not wait on them.

## Usage
When using [drachtio-fsrmf](https://www.npmjs.com/package/drachtio-fsmrf), you can access this API command via the api method on the 'endpoint' object.
```js
ep.api('uuid_aws_transcribe', `${ep.uuid} start en-US interim`);  
```

## Building
You will need to build the AWS C++ SDK.  You can use [this ansible role](https://github.com/davehorton/ansible-role-fsmrf), or refer to the specific steps [here](https://github.com/davehorton/ansible-role-fsmrf/blob/a1947cc24e89dee7d6b42053c53295f9198340c1/tasks/grpc.yml#L28).

## Examples
[aws_transcribe.js](../../examples/aws_transcribe.js)
