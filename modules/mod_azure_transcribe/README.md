# mod_azure_transcribe

A Freeswitch module that generates real-time transcriptions on a Freeswitch channel by using the Microsoft streaming transcription API

## API

### Commands
The freeswitch module exposes the following API commands:

```
uuid_azure_transcribe <uuid> start <lang-code> [interim] [stereo|mono] [bugname]
```
Attaches media bug to channel and performs streaming recognize request.
- `uuid` - unique identifier of Freeswitch channel
- `lang-code` - a valid Azure Speech [language code](https://learn.microsoft.com/en-us/azure/ai-services/speech-service/language-support) that is supported for streaming transcription
- `interim` - If the 'interim' keyword is present then both interim and final transcription results will be returned; otherwise only final transcriptions will be returned
- `stereo` - If the 'stereo' keyword is present, both caller and callee audio are captured as a two-channel (stereo) stream; otherwise only the caller's audio is captured
- `bugname` - optional name for the media bug (default: `azure_transcribe`); use the same name with `stop` to stop a specifically-named transcription

```
uuid_azure_transcribe <uuid> stop [bugname]
```
Stop transcription on the channel.

### Authentication
The plugin will first look for channel variables, then environment variables. A subscription key and region, or a service endpoint, are required: a start without any of them fails with an error event instead of silently connecting.

The names of the channel variables and environment variables are:

| variable | Description |
| --- | ----------- |
| AZURE_SUBSCRIPTION_KEY | The Azure subscription key |
| AZURE_REGION | The Azure region |
| AZURE_SERVICE_ENDPOINT | use this service endpoint instead of the region + key combination (env var or channel variable) |

### Channel variables
The following channel variables can be set to configure the Azure speech to text service

| variable | Description | Default |
| --- | ----------- |  ---|
| AZURE_PROFANITY_OPTION | "masked", "removed", "raw" | raw|
| AZURE_REQUEST_SNR | if set to 1 or true, enables signal to noise ratio reporting | off |
| AZURE_INITIAL_SPEECH_TIMEOUT_MS | initial time to wait for speech before returning no match | 180000 |
| AZURE_SPEECH_HINTS | comma-separated list of phrases or words to expect | none |
| AZURE_SPEECH_ALTERNATIVE_LANGUAGE_CODES | comma-separated list (max 3) of alternative languages for auto language detection; the start command's lang-code is the primary | none |
| AZURE_SPEECH_SEGMENTATION_SILENCE_TIMEOUT_MS | silence interval used to determine the end of an utterance | none (SDK default) |
| AZURE_USE_OUTPUT_FORMAT_DETAILED | if set to true or 1, provide n-best and confidence levels | off |
| AZURE_SERVICE_ENDPOINT_ID | custom speech model endpoint id | none |
| AZURE_AUDIO_LOGGING | if set, sends an audio log to the service | off |
| START_RECOGNIZING_ON_VAD | if set to 1 or true, do not begin streaming audio to Azure until voice activity is detected. On a **stereo** capture, note the VAD effectively monitors the read (caller) channel: FS's energy path strides per channel from the first, and the fvad path is mono-only — speech present only on the callee channel may not trigger the connect | off |
| RECOGNIZER_VAD_MODE | An integer value 0-3 from less to more aggressive vad detection | 2 |
| RECOGNIZER_VAD_SILENCE_MS | Milliseconds of silence before the VAD resets | 150 |
| RECOGNIZER_VAD_VOICE_MS | Milliseconds of voice activity required to trigger the connection to Azure when START_RECOGNIZING_ON_VAD is set | 250 |
| RECOGNIZER_VAD_DEBUG | if >0 vad debug logs will be generated | 0 |

Environment variables (not channel variables): `AZURE_SDK_LOGFILE` (write the Azure SDK log to a file), `JAMBONES_HTTP_PROXY_IP` / `JAMBONES_HTTP_PROXY_PORT` / `JAMBONES_HTTP_PROXY_USERNAME` / `JAMBONES_HTTP_PROXY_PASSWORD` (proxy for the SDK's websocket).

### Events
`azure_transcribe::transcription` - returns an interim or final transcription.  The event contains a JSON body describing the transcription result; if the body contains a property with "RecognitionStatus": "Success" it is a final transcript, otherwise it is an interim transcript.
```json
{
  "Id": "1708f0bffc2d4d66b8347280447e9dde",
  "RecognitionStatus": "Success",
  "DisplayText": "This is a test.",
  "Offset": 14400000,
  "Duration": 12200000
}
```

Other events fired by the module: `azure_transcribe::start_of_utterance` / `azure_transcribe::end_of_utterance` (speech detection), `azure_transcribe::no_speech_detected`, `azure_transcribe::vad_detected` (VAD path), and `jambonz_transcribe::error` on errors (the subclass name is inherited and frozen -- external consumers key on it). All events carry `transcription-vendor: microsoft` and `transcription-session-finished` headers.

## Usage
When using [drachtio-fsrmf](https://www.npmjs.com/package/drachtio-fsmrf), you can access this API command via the api method on the 'endpoint' object.
```js
ep.api('uuid_azure_transcribe', `${ep.uuid} start en-US interim`);  
```
