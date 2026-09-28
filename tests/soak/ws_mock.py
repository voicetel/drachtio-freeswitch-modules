#!/usr/bin/env python3
# Minimal mock WebSocket sink for mod_audio_fork / mod_deepgram_transcribe soak.
# Accepts connections, drains audio frames, optionally sends back a small JSON
# message periodically (to exercise the inbound receive path), and supports a
# "drop after N seconds" mode to simulate far-end disconnects.
#
# TLS: set WS_TLS_CERT + WS_TLS_KEY to serve wss. mod_deepgram_transcribe's
# AudioPipe dials LCCSCF_USE_SSL unconditionally, so without this the deepgram
# soak never leaves CONNECT_FAIL and none of its connected-path code runs.
import asyncio, ssl, sys, os
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
TLS_CERT = os.environ.get("WS_TLS_CERT", "")
TLS_KEY = os.environ.get("WS_TLS_KEY", "")


def tls_context():
    if not TLS_CERT or not TLS_KEY:
        return None
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=TLS_CERT, keyfile=TLS_KEY)
    return ctx


def pick_subprotocol(server, offered):
    # mod_audio_fork offers the audio.drachtio.org subprotocol; the
    # mod_deepgram_transcribe pipe offers NONE at all (its i.protocol assignment
    # is commented out in connect_client). websockets' default select_subprotocol
    # raises NegotiationError ("missing subprotocol", answered as HTTP 400) when a
    # client is configured with a list but offers none -- which silently reduced
    # the whole deepgram soak to CONNECT_FAIL. The real service keys off query
    # params and requires no subprotocol, so accept whatever is offered and fall
    # back to none.
    if not offered:
        return None
    return offered[0]


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
            # The real service closes the socket after it accepts a CloseStream
            # request, and the module's reaper is finish() + waitForClose() with
            # no close() of its own -- it depends on this remote close to
            # fulfill the promise. Model it, or every graceful teardown in the
            # soak blocks forever.
            if isinstance(msg, str) and "CloseStream" in msg:
                await ws.close()
                break
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
                # 2) an oversized message (> the module's 650KB MAX_RECV_BUF_SIZE)
                #    delivered as ONE frame: exercises the first-fragment path,
                #    which allocates len + lws_remaining_packet_payload up front
                try:
                    await ws.send('{"type":"transcription","data":"' + 'x' * (700 * 1024) + '"}')
                except Exception:
                    pass
    except Exception:
        pass

async def main():
    async with websockets.serve(handler, "127.0.0.1", PORT, max_size=None, ping_interval=None,
                                select_subprotocol=pick_subprotocol, ssl=tls_context()):
        sys.stderr.write("ws_mock listening on 127.0.0.1:%d (drop_after=%s tls=%s)\n"
                         % (PORT, DROP_AFTER, bool(TLS_CERT)))
        await asyncio.Future()

asyncio.run(main())
