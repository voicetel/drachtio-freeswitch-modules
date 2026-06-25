#!/usr/bin/env bash
# Run the AudioPipe concurrency soak under ASan and TSan against the mock ws.
set -u
cd /opt/soak
rc=0

run_one() {
  local label="$1" bin="$2"
  echo "================== ${label} =================="
  WS_PORT=9000 python3 ws_mock.py 9000 >/tmp/ws9000.log 2>&1 &  W1=$!
  WS_DROP_AFTER=0.3 WS_PORT=9001 python3 ws_mock.py 9001 >/tmp/ws9001.log 2>&1 & W2=$!
  sleep 1
  ITER="${ITER:-200}" WORKERS="${WORKERS:-4}" WS_PORT=9000 WS_DROP_PORT=9001 setarch -R ./"$bin"
  local r=$?
  kill $W1 $W2 2>/dev/null; wait $W1 $W2 2>/dev/null
  if [ $r -ne 0 ]; then echo ">>> ${label}: sanitizer/exit error (rc=$r)"; rc=1; else echo ">>> ${label}: clean (exit 0)"; fi
  echo
}

export ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:abort_on_error=1:detect_stack_use_after_return=1"
export TSAN_OPTIONS="halt_on_error=0:second_deadlock_stack=1:history_size=4"

run_one "ASan + UBSan + LeakSanitizer" soak_asan
run_one "ThreadSanitizer" soak_tsan

echo "================== OVERALL: $([ $rc -eq 0 ] && echo PASS || echo FAIL) =================="
exit $rc
