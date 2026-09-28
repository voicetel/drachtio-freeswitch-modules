#!/usr/bin/env bash
# Run the AudioPipe concurrency soak under ASan and TSan against the mock ws.
set -u
cd /opt/soak
rc=0

# mod_deepgram_transcribe's AudioPipe dials TLS unconditionally, so it needs a
# wss mock. Self-signed, throwaway: generated at run time, never committed, and
# trusted by the client only via SSL_CERT_FILE below (the pipe itself sets no
# CA path and no skip-verify flag).
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout /opt/soak/tls.key -out /opt/soak/tls.crt \
  -days 2 -subj "/CN=127.0.0.1" -addext "subjectAltName=IP:127.0.0.1" \
  >/dev/null 2>&1 || { echo "FATAL: cert generation failed"; exit 2; }

run_one() {
  local label="$1" bin="$2" need_drop_mock="${3:-1}" use_tls="${4:-0}"
  echo "================== ${label} =================="
  local tls_env=()
  if [ "$use_tls" = "1" ]; then
    # OpenSSL's default verify paths honour SSL_CERT_FILE, which is how the
    # client trusts the self-signed mock without any module-side change.
    tls_env=(WS_TLS_CERT=/opt/soak/tls.crt WS_TLS_KEY=/opt/soak/tls.key)
    export SSL_CERT_FILE=/opt/soak/tls.crt
  else
    unset SSL_CERT_FILE
  fi
  # WS_OVERSIZED_EVERY: the mocks also send fragmented + >650KB inbound
  # messages, exercising the recv reassembly and oversized-discard paths
  # (regression coverage for the c31364f class). The mock's frame counter is
  # PER CONNECTION and a soak connection lives only ~20-50 frames, so this
  # must be well under that or the mode never fires.
  env "${tls_env[@]}" WS_OVERSIZED_EVERY="${WS_OVERSIZED_EVERY:-25}" WS_PORT=9000 python3 ws_mock.py 9000 >/tmp/ws9000.log 2>&1 &  W1=$!
  W2=""
  if [ "$need_drop_mock" = "1" ]; then
    env "${tls_env[@]}" WS_OVERSIZED_EVERY="${WS_OVERSIZED_EVERY:-25}" WS_DROP_AFTER=0.3 WS_PORT=9001 python3 ws_mock.py 9001 >/tmp/ws9001.log 2>&1 & W2=$!
  fi
  sleep 1
  ITER="${ITER:-200}" WORKERS="${WORKERS:-4}" WS_PORT=9000 WS_DROP_PORT=9001 setarch -R ./"$bin"
  local r=$?
  kill $W1 $W2 2>/dev/null; wait $W1 $W2 2>/dev/null
  if [ $r -ne 0 ]; then echo ">>> ${label}: sanitizer/exit error (rc=$r)"; rc=1; else echo ">>> ${label}: clean (exit 0)"; fi
  echo
}

export ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:abort_on_error=1:detect_stack_use_after_return=1"
export TSAN_OPTIONS="halt_on_error=0:second_deadlock_stack=1:history_size=4"

run_one "mod_audio_fork AudioPipe: ASan + UBSan + LeakSanitizer" soak_asan
run_one "mod_audio_fork AudioPipe: ThreadSanitizer" soak_tsan

# deepgram dials wss always, so both instances serve TLS and the handshakes
# complete: the connected path (ESTABLISHED, inbound recv reassembly and
# oversized discard, CloseStream teardown, far-end drop) is exercised rather
# than only CONNECT_FAIL. The 9001 drop instance is used again for real.
run_one "mod_deepgram_transcribe AudioPipe: ASan + UBSan + LeakSanitizer" soak_dg_asan 1 1
# lws-internal logging races are suppressed for the deepgram run (see tsan.supp;
# our own frames are never suppressed by a called_from_lib rule)
SAVED_TSAN="$TSAN_OPTIONS"
export TSAN_OPTIONS="$TSAN_OPTIONS:suppressions=/opt/soak/tsan.supp"
run_one "mod_deepgram_transcribe AudioPipe: ThreadSanitizer" soak_dg_tsan 1 1
export TSAN_OPTIONS="$SAVED_TSAN"

echo "================== OVERALL: $([ $rc -eq 0 ] && echo PASS || echo FAIL) =================="
exit $rc
