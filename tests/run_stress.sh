#!/usr/bin/env bash
#
# Stress / robustness tests for jt9_decode.
#
#   tests/run_stress.sh <jt9_decode> <jt9_decode_asan> <jt9_decode_tsan>
#
# Every run must end with a normal exit (never a signal / abort / sanitizer
# report), and leave no jt9 process, shared memory, semaphore or temp dir.
#
# Environment: JT9=<path> (default: jt9 from PATH), FUZZ_N (default 400),
#              SOAK_S (default 120)
#
set -u

BIN=$(readlink -f "$1")
ASAN=$(readlink -f "$2")
TSAN=$(readlink -f "$3")
JT9=${JT9:-$(command -v jt9)}
FUZZ_N=${FUZZ_N:-400}
SOAK_S=${SOAK_S:-120}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
WAV=$ROOT/test_ft8.wav

WORK=$(mktemp -d "${TMPDIR:-/tmp}/jt9_decode_stress.XXXXXX")
trap 'pkill -f "sleep.3019" 2>/dev/null; rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1

export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
export TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1

pass=0
fail=0
ok()  { pass=$((pass + 1)); echo "  ok   $*"; }
bad() { fail=$((fail + 1)); echo "  FAIL $*"; }

leftovers() {
    local found=""
    found+=$(ls /tmp 2>/dev/null | grep -E '^qipc_(sharedmemory|systemsem)_JTDECODE' | head -3)
    found+=$(ls /dev/shm 2>/dev/null | grep -E '^jt9_decode_[0-9]+_[0-9]+$' | head -3)
    found+=$(pgrep -af -- "-s JT9DECODE_" | head -3)
    found+=$(pgrep -af "fake_jt9" | grep -v pgrep | head -3)
    echo -n "$found"
}
check_clean() {
    local l
    l=$(leftovers)
    if [ -z "$l" ]; then ok "$1: nothing left behind"; else bad "$1: left behind: $l"; fi
}
# A run is acceptable if it exited normally (0/1) and no sanitizer fired
check_run() {   # name rc errfile
    if [ "$2" -gt 1 ]; then bad "$1: exit code $2"; tail -15 "$3"; return 1; fi
    if grep -qE 'ERROR: (Address|Leak|Thread)Sanitizer|runtime error:|WARNING: ThreadSanitizer' "$3"; then
        bad "$1: sanitizer report"; grep -A15 -E 'Sanitizer|runtime error' "$3" | head -30; return 1
    fi
    return 0
}
wait_for() {
    local end=$((SECONDS + $3))
    while [ $SECONDS -lt $end ]; do grep -qE "$2" "$1" 2>/dev/null && return 0; sleep 0.2; done
    return 1
}
# Stop a stream-mode run: SIGTERM, wait, kill the stdin feeder, collect rc
stop_run() {    # pid
    kill -TERM "$1" 2>/dev/null
    local n=0
    while kill -0 "$1" 2>/dev/null && [ $n -lt 100 ]; do sleep 0.1; n=$((n + 1)); done
    if kill -0 "$1" 2>/dev/null; then kill -9 "$1"; echo "HUNG" >&2; fi
}

if [ -n "$(leftovers)" ]; then echo "Refusing to run: leftovers exist:"; leftovers; echo; exit 1; fi

sox "$WAV" -t raw -r 12000 -e signed -b 16 -c 1 ft8.raw

# Fake jt9s: each misbehaves in a different way (none attaches to shared memory)
mkdir fake
cat >fake/fake_jt9_exit <<'EOF'
#!/bin/sh
exit 0
EOF
cat >fake/fake_jt9_hang <<'EOF'
#!/bin/sh
exec sleep 3019
EOF
cat >fake/fake_jt9_spew <<'EOF'
#!/usr/bin/env python3
# Hostile output: binary junk, huge lines, lines without newline, malformed markers
import os, random, sys, time
out = sys.stdout.buffer
r = random.Random(int(sys.argv[-1].split('_')[-1]) if '_' in sys.argv[-1] else 1)
lines = [b'<DecodeFinished>', b'<DecodeFinished>   x', b'<DecodeFinished>   1  999999999999999999999 0',
         b'<DecodeFinished>   0  -5        0', b'1234567 decode-looking line', b'\x00\x01\x02\xff' * 50,
         b'9' * 300000, b'%s%n%x%s%n', b'', b'\r\r\r', b'<', b'0000000']
end = time.time() + 600
while time.time() < end:
    c = r.random()
    if c < 0.6:
        out.write(r.choice(lines) + b'\n')
    elif c < 0.8:
        out.write(os.urandom(r.randint(1, 5000)))
    elif c < 0.85:
        out.write(b'A' * 3000000)          # > 1MB without newline
    else:
        out.flush(); time.sleep(r.random() * 0.2)
EOF
chmod +x fake/*

# ---- 1. WAV fuzzing (ASan+UBSan) -------------------------------------------------
echo "1. WAV reader fuzzing under ASan/UBSan ($FUZZ_N files)"
python3 - "$WAV" "$FUZZ_N" <<'EOF'
import os, random, struct, sys
src = open(sys.argv[1], 'rb').read()
n = int(sys.argv[2])
r = random.Random(12345)
os.makedirs('fuzz', exist_ok=True)
for i in range(n):
    d = bytearray(src[:4096 + r.randint(0, 8192)])
    kind = i % 8
    if kind == 0:       # random byte flips in the header
        for _ in range(r.randint(1, 12)):
            d[r.randint(0, 63)] = r.randint(0, 255)
    elif kind == 1:     # extreme field values
        off = r.choice([4, 16, 22, 24, 28, 32, 34, 40])
        size = 2 if off in (22, 32, 34) else 4
        d[off:off + size] = r.choice([b'\xff' * size, b'\x00' * size, b'\x7f' + b'\xff' * (size - 1)])
    elif kind == 2:     # truncate anywhere
        d = d[:r.randint(0, 64)]
    elif kind == 3:     # stereo / odd channel counts with tiny data
        d[22:24] = struct.pack('<H', r.choice([0, 2, 3, 255, 65535]))
    elif kind == 4:     # many bogus chunks before data, odd sizes
        chunks = b''.join(os.urandom(4) + struct.pack('<I', r.randint(0, 40)) + os.urandom(r.randint(0, 40))
                          for _ in range(r.randint(1, 50)))
        d = d[:36] + chunks + d[36:]
    elif kind == 5:     # chunk size pointing far past EOF
        d = d[:36] + b'JUNK' + struct.pack('<I', 0xfffffff0) + d[36:]
    elif kind == 6:     # data chunk claiming 4GB
        d[40:44] = b'\xff\xff\xff\xff'
    else:               # completely random
        d = bytearray(os.urandom(r.randint(0, 200)))
        if r.random() < 0.5: d[0:4] = b'RIFF'; d[8:12] = b'WAVE'
    open('fuzz/%04d.wav' % i, 'wb').write(bytes(d))
# Also a file larger than the 30 minute buffer
big = src[:44] + bytes(NT := 12000 * 60 * 31 * 2)
big = bytearray(big); big[40:44] = struct.pack('<I', NT); big[4:8] = struct.pack('<I', len(big) - 8)
open('fuzz/huge.wav', 'wb').write(bytes(big))
EOF
fuzz_fail=0
run_fuzz() {
    local f=$1
    "$ASAN" -j "$WORK/fake/fake_jt9_exit" -m FT8 "$f" >"$f.out" 2>"$f.err"
    local rc=$?
    if [ $rc -gt 1 ] || grep -qE 'Sanitizer|runtime error:' "$f.err"; then echo "$f rc=$rc"; fi
}
export -f run_fuzz
export ASAN WORK
ls fuzz/*.wav | xargs -P 16 -I{} bash -c 'run_fuzz {}' >fuzz_failures.txt
if [ -s fuzz_failures.txt ]; then
    bad "WAV fuzz: $(wc -l <fuzz_failures.txt) crashing inputs"; head -5 fuzz_failures.txt
    f=$(head -1 fuzz_failures.txt | cut -d' ' -f1); grep -A20 -E 'Sanitizer|runtime error' "$f.err" | head -25
else
    ok "WAV fuzz: $(ls fuzz/*.wav | wc -l) inputs, no crash / sanitizer report"
fi
check_clean "WAV fuzz"

# Same with the real jt9 on a sample (it really reads the audio)
ls fuzz/*.wav | shuf -n 24 --random-source=<(yes) >real_sample.txt
real_fail=0
while read -r f; do
    timeout 60 "$ASAN" -j "$JT9" -m FT8 "$f" >"$f.rout" 2>"$f.rerr"
    rc=$?
    check_run "fuzz+real jt9 $f" $rc "$f.rerr" >/dev/null || { real_fail=1; echo "  $f rc=$rc"; }
done < real_sample.txt
[ $real_fail = 0 ] && ok "WAV fuzz with real jt9: 24 inputs ok" || bad "WAV fuzz with real jt9"
check_clean "WAV fuzz real jt9"

# ---- 2. Real decodes under ASan/UBSan --------------------------------------------
echo "2. Normal decodes under ASan/UBSan"
"$ASAN" -j "$JT9" -m FT8 "$WAV" >asan_wav.out 2>asan_wav.err
rc=$?
check_run "ASan WAV decode" $rc asan_wav.err && [ "$(grep -cE '^[0-9]{6} ' asan_wav.out)" = 10 ] \
    && ok "ASan WAV decode: 10 decodes, clean" || bad "ASan WAV decode: $(grep -cE '^[0-9]{6} ' asan_wav.out) decodes"
check_clean "ASan WAV decode"

# stream helper: name binary input_cmd need_regex timeout -- args
stream_run() {
    local name=$1 bin=$2 input=$3 need=$4 tmo=$5; shift 5
    bash -c "$input" 2>/dev/null | "$bin" "$@" >"$name.out" 2>"$name.err" &
    local pid=$!
    if wait_for "$name.out" "$need" "$tmo" || wait_for "$name.err" "$need" 1; then
        ok "$name: reached '$need'"
    else
        bad "$name: never reached '$need'"; tail -5 "$name.err"
    fi
    stop_run $pid 2>>"$name.err"
    pkill -f "sleep.3019" 2>/dev/null
    wait $pid 2>/dev/null
    local rc=$?
    grep -q HUNG "$name.err" && bad "$name: hung on SIGTERM"
    check_run "$name" $rc "$name.err" && ok "$name: exit $rc, no sanitizer report"
    check_clean "$name"
}

stream_run asan_stream_ft8 "$ASAN" "cat ft8.raw; sleep 3019" "cycle_num=1 .*num_decodes=10 " 60 -m FT8 -s -j "$JT9"
stream_run asan_stream_ft2 "$ASAN" "cat ft8.raw; sleep 3019" "cycle_num=4 " 60 -m FT2 -s -j "$JT9"

# ---- 3. Hostile stdin ---------------------------------------------------------------
echo "3. Hostile stdin under ASan/UBSan"
stream_run asan_random_audio "$ASAN" "head -c 2000000 /dev/urandom; sleep 3019" "cycle_num=3 " 60 -m FT2 -s -j "$JT9"
stream_run asan_odd_bytes "$ASAN" "python3 -c \"
import sys,time,os
o=sys.stdout.buffer
for i in range(30000):
    o.write(os.urandom(1 + (i % 7) * 2)); o.flush()
time.sleep(3019)\"" "cycle_num=2 " 60 -m FT2 -s -j "$JT9"
stream_run asan_empty_stdin "$ASAN" "true" "Waiting for first cycle boundary" 10 -m FT8 -s -j "$JT9"
stream_run asan_tiny_stdin "$ASAN" "printf x; sleep 3019" "Waiting for first cycle boundary" 10 -m FT2 -s -j "$JT9"

# ---- 4. Misbehaving jt9 -------------------------------------------------------------
echo "4. Misbehaving jt9 under ASan/UBSan"
# never answers: watchdog must fire and recover repeatedly
stream_run asan_jt9_hang "$ASAN" "cat ft8.raw; sleep 3019" "watchdog fired \\(total: 2\\)" 60 -m FT2 -s -j "$WORK/fake/fake_jt9_hang"
# exits right away: must be noticed and reported
cat ft8.raw | "$ASAN" -m FT2 -s -j "$WORK/fake/fake_jt9_exit" >asan_jt9_exit.out 2>asan_jt9_exit.err &
p=$!; n=0; while kill -0 $p 2>/dev/null && [ $n -lt 100 ]; do sleep 0.1; n=$((n+1)); done
kill -0 $p 2>/dev/null && { bad "jt9 exits at start: wrapper still running"; kill -9 $p; }
wait $p; rc=$?
check_run "jt9 exits at start" $rc asan_jt9_exit.err && grep -q 'exited unexpectedly' asan_jt9_exit.err \
    && ok "jt9 exits at start: reported, exit $rc" || bad "jt9 exits at start: not reported"
check_clean "jt9 exits at start"
# hostile output for 20s
stream_run asan_jt9_spew "$ASAN" "cat ft8.raw; sleep 3019" "cycle_num=3 " 60 -m FT2 -s -j "$WORK/fake/fake_jt9_spew"
# jt9 killed at random moments
kill_fail=0
for i in 1 2 3 4 5; do
    (cat ft8.raw; sleep 3019) | "$ASAN" -m FT2 -s -j "$JT9" >kill_$i.out 2>kill_$i.err &
    p=$!
    sleep "$(python3 -c "import random;print(round(random.uniform(0.2,8),2))")"
    pkill -9 -f -- "-s JT9DECODE_${p}_"
    n=0; while kill -0 $p 2>/dev/null && [ $n -lt 100 ]; do sleep 0.1; n=$((n+1)); done
    kill -0 $p 2>/dev/null && { bad "jt9 killed #$i: wrapper hung"; kill -9 $p; kill_fail=1; }
    pkill -f "sleep.3019"; wait $p; rc=$?
    check_run "jt9 killed #$i" $rc kill_$i.err || kill_fail=1
done
[ $kill_fail = 0 ] && ok "jt9 killed at random moments: 5 runs, wrapper always exited normally"
check_clean "jt9 killed at random"

# ---- 5. Signals at random moments ----------------------------------------------------
echo "5. SIGTERM/SIGINT at random moments (25 runs, ASan)"
sig_fail=0
for i in $(seq 25); do
    sig=$([ $((i % 2)) = 0 ] && echo TERM || echo INT)
    (cat ft8.raw; sleep 3019) | "$ASAN" -m FT2 -s -j "$JT9" >sig_$i.out 2>sig_$i.err &
    p=$!
    sleep "$(python3 -c "import random;print(round(random.choice([0,0.01,0.05,0.2,0.5,1,2,4,6]) + random.random()*0.05,3))")"
    kill -$sig $p 2>/dev/null
    n=0; while kill -0 $p 2>/dev/null && [ $n -lt 100 ]; do sleep 0.1; n=$((n+1)); done
    kill -0 $p 2>/dev/null && { bad "signal run $i: hung"; kill -9 $p; sig_fail=1; }
    pkill -f "sleep.3019"; wait $p; rc=$?
    check_run "signal run $i ($sig)" $rc sig_$i.err || sig_fail=1
    [ -n "$(leftovers)" ] && { bad "signal run $i: left behind: $(leftovers)"; sig_fail=1; break; }
done
[ $sig_fail = 0 ] && ok "25 signals at random moments: clean exit, nothing left behind"

# WAV mode interrupted too
for d in 0.05 0.5 1.5 2.05; do
    "$ASAN" -j "$JT9" -m FT8 "$WAV" >wsig.out 2>wsig.err &
    p=$!; sleep $d; kill -TERM $p
    t0=$SECONDS; wait $p; rc=$?
    check_run "WAV SIGTERM at ${d}s" $rc wsig.err && [ $((SECONDS - t0)) -le 3 ] \
        && ok "WAV SIGTERM at ${d}s: exit $rc within 3s" || bad "WAV SIGTERM at ${d}s: slow or failed"
done
check_clean "WAV mode SIGTERM"

# ---- 6. Closed stdout (downstream went away) ------------------------------------------
echo "6. Closed stdout"
(cat ft8.raw; sleep 3019) | "$ASAN" -m FT8 -s -j "$JT9" 2>pipe.err | head -c 1 >/dev/null &
for _ in $(seq 300); do grep -q 'cannot write to stdout' pipe.err && break; sleep 0.1; done
pkill -f "sleep.3019"; wait
grep -q 'cannot write to stdout' pipe.err && ok "closed stdout: detected and shut down" || bad "closed stdout: not handled"
check_run "closed stdout" 0 pipe.err
check_clean "closed stdout"

# ---- 7. Data races (TSan) --------------------------------------------------------------
echo "7. ThreadSanitizer"
# TSan aborts with "unexpected memory mapping" under the high-entropy ASLR of
# recent kernels; run it with ASLR disabled (exec keeps the PID for signals)
printf '#!/bin/sh\nexec setarch "$(uname -m)" -R "%s" "$@"\n' "$TSAN" >tsan_run
chmod +x tsan_run
stream_run tsan_stream_ft2 "$WORK/tsan_run" "python3 -c \"
import sys,time
d=open('ft8.raw','rb').read()
o=sys.stdout.buffer
while True:
    for i in range(0,len(d),2400): o.write(d[i:i+2400]); o.flush(); time.sleep(0.1)
\"" "cycle_num=4 " 60 -m FT2 -s -j "$JT9"
pkill -f "range\(0,len\(d\),2400\)" 2>/dev/null

# ---- 8. Many concurrent instances --------------------------------------------------------
echo "8. 8 concurrent stream instances + 8 concurrent WAV decodes"
pids=()
for i in $(seq 8); do
    (cat ft8.raw; sleep 3019) | "$BIN" -m FT2 -s -j "$JT9" >conc_$i.out 2>conc_$i.err &
    pids+=($!)
done
for i in $(seq 8); do "$BIN" -j "$JT9" -m FT8 "$WAV" >concw_$i.out 2>concw_$i.err & done
for i in $(seq 8); do wait_for conc_$i.out "cycle_num=3 " 60 || bad "concurrent stream $i stalled"; done
for p in "${pids[@]}"; do stop_run $p; done
pkill -f "sleep.3019"; wait
conc_ok=1
for i in $(seq 8); do
    [ "$(grep -cE '^[0-9]{6} ' concw_$i.out)" = 10 ] || { bad "concurrent WAV $i: $(grep -cE '^[0-9]{6} ' concw_$i.out) decodes"; conc_ok=0; }
done
[ $conc_ok = 1 ] && ok "concurrent: 8 streams reached cycle 3, 8 WAV runs each got all 10 decodes"
check_clean "concurrent"

# ---- 9. Soak: real-time stream -------------------------------------------------------------
echo "9. Soak: ${SOAK_S}s real-time FT8 stream under ASan"
python3 - <<'EOF' >soak_gen.py
print('''
import sys, time
d = open("ft8.raw", "rb").read()
o = sys.stdout.buffer
t0 = time.time(); sent = 0
while True:
    for i in range(0, len(d), 1200):
        o.write(d[i:i+1200]); o.flush(); sent += 1200
        ahead = sent / 24000.0 - (time.time() - t0)
        if ahead > 0: time.sleep(ahead)
''')
EOF
python3 soak_gen.py 2>/dev/null | "$ASAN" -m FT8 -s -j "$JT9" >soak.out 2>soak.err &
p=$!
sleep "$SOAK_S"
cycles=$(grep -c '^<DecodeStats>' soak.out)
skipped=$(grep -o 'skipped_cycles=[0-9]*' soak.out | tail -1)
stop_run $p 2>>soak.err
pkill -f soak_gen.py; wait $p; rc=$?
check_run "soak" $rc soak.err && ok "soak: $cycles decode cycles, $skipped, no watchdog: $(grep -c 'watchdog fired' soak.err)"
expected=$((SOAK_S / 15 - 2))
[ "$cycles" -ge "$expected" ] && ok "soak: at least $expected cycles" || bad "soak: only $cycles cycles"
grep -q 'watchdog fired' soak.err && bad "soak: watchdog fired"
check_clean "soak"

echo
echo "$pass passed, $fail failed"
[ $fail = 0 ]
