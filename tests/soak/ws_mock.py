#!/usr/bin/env python3
# Minimal mock WebSocket sink for mod_audio_fork / mod_deepgram_transcribe soak.
# Accepts connections, drains audio frames, optionally sends back a small JSON
# message periodically (to exercise the inbound receive path), and supports a
# "drop after N seconds" mode to simulate far-end disconnects.
import asyncio, sys, os
try:
    import websockets
except Exception as e:
    sys.stderr.write("FATAL: python websockets not available: %s\n" % e)
    sys.exit(2)

PORT = int(os.environ.get("WS_PORT", sys.argv[1] if len(sys.argv) > 1 else "9000"))
DROP_AFTER = float(os.environ.get("WS_DROP_AFTER", "0"))  # 0 = never drop
# every N received frames, also send a fragmented message and an oversized
# one (0 = disabled). Exercises the module's inbound paths that plain small
# messages never reach: fragment reassembly, the >650KB MAX_RECV_BUF_SIZE
# discard path, and its truncated-tail handling (regression coverage for
# the c31364f class).
OVERSIZED_EVERY = int(os.environ.get("WS_OVERSIZED_EVERY", "0"))

async def handler(*args):
    ws = args[0]
    n = 0
    try:
        if DROP_AFTER > 0:
            async def killer():
                await asyncio.sleep(DROP_AFTER)
                await ws.close()
            asyncio.ensure_future(killer())
        async for msg in ws:
            n += 1
            # occasionally push an inbound JSON message back to the module to
            # exercise its receive/parse path (mod_audio_fork handles playAudio etc.)
            if n % 200 == 0:
                try:
                    await ws.send('{"type":"transcription","data":{"is_final":false,"text":"soak"}}')
                except Exception:
                    break
            if OVERSIZED_EVERY > 0 and n % OVERSIZED_EVERY == 0:
                # 1) a deliberately fragmented valid message: sending an iterable
                #    makes the library emit continuation frames, exercising the
                #    recv-buffer reassembly path
                try:
                    await ws.send(['{"type":"transcription","data":{"is_final":false,',
                                   '"text":"fragmented soak"}}'])
                except Exception:
                    pass
                # 2) an oversized message (> the module's 650KB MAX_RECV_BUF_SIZE):
                #    exercises the max-buffer-exceeded discard path
                try:
                    await ws.send('{"type":"transcription","data":"' + 'x' * (700 * 1024) + '"}')
                except Exception:
                    pass
    except Exception:
        pass

async def main():
    # negotiate the audio.drachtio.org subprotocol that the libwebsockets client offers
    async with websockets.serve(handler, "127.0.0.1", PORT, max_size=None, ping_interval=None,
                                subprotocols=["audio.drachtio.org"]):
        sys.stderr.write("ws_mock listening on 127.0.0.1:%d (drop_after=%s)\n" % (PORT, DROP_AFTER))
        await asyncio.Future()

asyncio.run(main())
