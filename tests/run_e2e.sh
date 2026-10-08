#!/usr/bin/env bash
#
# End-to-end tests for jt9_decode against the real jt9.
#
#   tests/run_e2e.sh <jt9_decode binary>
#
# Environment:
#   JT9=<path>   jt9 to use (default: jt9 from PATH)
#   REF=<path>   optional reference build (e.g. the old Qt version). When set,
#                every WAV/error case is run on both binaries and exit code,
#                stdout decodes and normalised stderr must match exactly.
#   RECORD=1     rewrite tests/golden/* from the binary under test
#   QUICK=1      skip the slow stream-mode tests
#
set -u

BIN=$(readlink -f "$1")
REF=${REF:+$(readlink -f "$REF")}
JT9=${JT9:-$(command -v jt9)}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
GOLDEN=$HERE/golden
WAV=$ROOT/test_ft8.wav

WORK=$(mktemp -d "${TMPDIR:-/tmp}/jt9_decode_e2e.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1   # jt9 writes timer.out / wisdom files into its cwd

pass=0
fail=0
ok()   { pass=$((pass + 1)); echo "  ok   $*"; }
bad()  { fail=$((fail + 1)); echo "  FAIL $*"; }

# Decode text without the time column (which is the current UTC), sorted
decodes() { grep -E '^[0-9]{6} ' "$1" | cut -c7- | sed 's/[[:space:]]*$//' | LC_ALL=C sort; }

# stderr with run-specific values replaced. "Cleaned up temp directory" is
# dropped because the Qt build skipped that cleanup on error exits (a leak);
# check_clean verifies the new build really cleans up. "--version" is a new
# option the Qt build didn't have.
normalise() {
    sed -E -e 's/JT9DECODE_[0-9]+_[0-9]+/<KEY>/g' \
           -e 's#/dev/shm/jt9_decode_[0-9]+_[0-9]+#<TMP>#g' \
           -e 's/UTC: [0-9]{4}/UTC: <T>/' \
           -e '/^Cleaned up temp directory: /d' \
           -e '/^  --version     Show version$/d' \
           -e "s#$BIN#<BIN>#g" -e "s#${REF:-/nonexistent}#<BIN>#g" "$1"
}

# Nothing may be left behind by a run: IPC key files, temp dirs, jt9 children
leftovers() {
    local found=""
    found+=$(ls /tmp 2>/dev/null | grep -E '^qipc_(sharedmemory|systemsem)_JTDECODE' | head -3)
    found+=$(ls /dev/shm 2>/dev/null | grep -E '^jt9_decode_[0-9]+_[0-9]+$' | head -3)
    found+=$(pgrep -af -- "-s JT9DECODE_" | head -3)
    echo -n "$found"
}

check_clean() {
    local l
    l=$(leftovers)
    if [ -z "$l" ]; then ok "$1: nothing left behind"; else bad "$1: left behind: $l"; fi
}

# Run a WAV-mode case: name, args...
run_case() {
    local bin=$1 tag=$2; shift 2
    timeout 60 "$bin" "$@" >"$tag.out" 2>"$tag.err"
    echo $? >"$tag.rc"
}

# Compare a case between BIN and REF (exit code, decodes, normalised stderr)
diff_case() {
    local name=$1; shift
    run_case "$BIN" "new_$name" "$@"
    check_clean "$name"
    [ -z "$REF" ] && return 0
    run_case "$REF" "ref_$name" "$@"
    # The old Qt build doesn't clean up after some errors; don't let that leak
    cleanup_stray >/dev/null
    if [ "$(cat new_$name.rc)" = "$(cat ref_$name.rc)" ]; then
        ok "$name: exit code $(cat new_$name.rc) matches reference"
    else
        bad "$name: exit code $(cat new_$name.rc) vs reference $(cat ref_$name.rc)"
    fi
    if diff <(decodes new_$name.out) <(decodes ref_$name.out) >/dev/null &&
       diff <(grep -vE '^[0-9]{6} ' new_$name.out) <(grep -vE '^[0-9]{6} ' ref_$name.out) >/dev/null; then
        ok "$name: stdout matches reference"
    else
        bad "$name: stdout differs from reference"; diff new_$name.out ref_$name.out | head -10
    fi
    if diff <(normalise new_$name.err) <(normalise ref_$name.err) >/dev/null; then
        ok "$name: stderr matches reference"
    else
        bad "$name: stderr differs from reference"; diff <(normalise new_$name.err) <(normalise ref_$name.err) | head -20
    fi
}

# Remove leftovers of a killed process (only used for the reference build)
cleanup_stray() {
    pkill -9 -f -- "-s JT9DECODE_" 2>/dev/null
    for f in /tmp/qipc_sharedmemory_JTDECODE* /tmp/qipc_systemsem_JTDECODE*; do
        [ -e "$f" ] || continue
        local k
        k=$(python3 -c "import os,sys;s=os.stat(sys.argv[1]);print(hex((0x51<<24)|((s.st_dev&0xff)<<16)|(s.st_ino&0xffff)))" "$f")
        case $f in *sharedmemory*) ipcrm -M "$k" 2>/dev/null;; *) ipcrm -S "$k" 2>/dev/null;; esac
        rm -f "$f"
    done
    rm -rf /dev/shm/jt9_decode_[0-9]*_[0-9]*
}

expect_golden() {
    local name=$1 got=$2
    if [ "${RECORD:-}" = 1 ]; then
        mkdir -p "$GOLDEN"; decodes "$got" >"$GOLDEN/$name.txt"; echo "  recorded $GOLDEN/$name.txt"; return
    fi
    if diff <(decodes "$got") "$GOLDEN/$name.txt" >/dev/null; then
        ok "$name: $(decodes "$got" | wc -l) decodes match golden"
    else
        bad "$name: decodes differ from golden"; diff <(decodes "$got") "$GOLDEN/$name.txt" | head -20
    fi
}

if [ -n "$(leftovers)" ]; then
    echo "Refusing to run: leftovers from an earlier run exist:"; leftovers; echo; exit 1
fi

echo "jt9_decode: $BIN"
echo "jt9:        $JT9"
[ -n "$REF" ] && echo "reference:  $REF"
if ldd "$BIN" | grep -qi qt; then bad "binary links against Qt"; else ok "binary does not link against Qt"; fi

# ---- Test inputs -------------------------------------------------------------
sox "$WAV" -c 2 stereo.wav
sox "$WAV" -t raw -r 12000 -e signed -b 16 -c 1 ft8.raw
python3 - "$WAV" <<'EOF'
import struct, sys
data = open(sys.argv[1], 'rb').read()
fmt = data[12:36]              # 'fmt ' + size + 16 bytes
body = data[36:]               # 'data' chunk onwards
# fmt chunk with 2 extension bytes (fmt_size 18), and a LIST chunk before data
fmt18 = b'fmt ' + struct.pack('<I', 18) + fmt[8:] + b'\x00\x00'
lst = b'LIST' + struct.pack('<I', 12) + b'INFOISFT\x00\x00\x00\x00'
out = fmt18 + lst + body
open('extra_chunks.wav', 'wb').write(b'RIFF' + struct.pack('<I', 4 + len(out)) + b'WAVE' + out)
# Truncated audio: header promises full size, file ends half way
half = data[:44 + 90000]
open('truncated.wav', 'wb').write(half)
# Header only
open('header_only.wav', 'wb').write(data[:44])
# No data chunk at all
open('no_data.wav', 'wb').write(data[:36])
# Not a WAV
open('garbage.wav', 'wb').write(b'hello world, definitely not a wav file' * 10)
open('empty.wav', 'wb').write(b'')
EOF
printf '#!/bin/sh\nexit 3\n' >not_executable_jt9; chmod 644 not_executable_jt9
printf '#!/bin/sh\nexit 3\n' >exits_immediately; chmod 755 exits_immediately

# ---- WAV mode ----------------------------------------------------------------
echo "WAV mode"
diff_case ft8_mono -j "$JT9" -m FT8 "$WAV"
expect_golden ft8_wav new_ft8_mono.out
diff_case ft8_stereo -j "$JT9" -m FT8 stereo.wav
expect_golden ft8_wav new_ft8_stereo.out
diff_case ft8_extra_chunks -j "$JT9" -m ft8 extra_chunks.wav
expect_golden ft8_wav new_ft8_extra_chunks.out
grep -q 'Skipping chunk "LIST" (12 bytes)' new_ft8_extra_chunks.err && ok "LIST chunk skipped" || bad "LIST chunk not reported"
diff_case ft8_depth1 -j "$JT9" -m FT8 -d 1 "$WAV"
expect_golden ft8_wav_depth1 new_ft8_depth1.out
diff_case ft8_multithread -j "$JT9" -m FT8 -t "$WAV"
expect_golden ft8_wav new_ft8_multithread.out
diff_case ft8_truncated -j "$JT9" -m FT8 truncated.wav
diff_case ft8_header_only -j "$JT9" -m FT8 header_only.wav
diff_case ft4_wav -j "$JT9" -m FT4 "$WAV"
diff_case ft2_wav -j "$JT9" -m FT2 "$WAV"
diff_case default_mode -j "$JT9" "$WAV"

# ---- Argument / error handling ------------------------------------------------
echo "Errors"
diff_case help --help
diff_case no_args
diff_case no_jt9 "$WAV"
diff_case bad_mode -j "$JT9" -m FT9 "$WAV"
diff_case stream_and_wav -j "$JT9" -s "$WAV"
diff_case unknown_option -j "$JT9" --bogus "$WAV"
diff_case jt9_missing -j /nonexistent/jt9 "$WAV"
diff_case wav_missing -j "$JT9" -m FT8 /nonexistent.wav
diff_case wav_garbage -j "$JT9" -m FT8 garbage.wav
diff_case wav_empty -j "$JT9" -m FT8 empty.wav
diff_case wav_no_data -j "$JT9" -m FT8 no_data.wav
diff_case jt9_not_executable -j ./not_executable_jt9 -m FT8 "$WAV"
diff_case jt9_exits -j ./exits_immediately -m FT8 "$WAV"
diff_case jt9_is_dir -j /tmp -m FT8 "$WAV"

if [ "${QUICK:-}" = 1 ]; then
    echo; echo "$pass passed, $fail failed (stream tests skipped)"; [ $fail = 0 ]; exit
fi

# ---- Stream mode ---------------------------------------------------------------
echo "Stream mode"

# wait_for <file> <regex> <timeout_s>
wait_for() {
    local end=$((SECONDS + $3))
    while [ $SECONDS -lt $end ]; do
        grep -qE "$2" "$1" 2>/dev/null && return 0
        sleep 0.2
    done
    return 1
}

# stream_case <name> <stats_needed> <input cmd> -- args
stream_case() {
    local name=$1 need=$2 input=$3; shift 3
    bash -c "$input" | "$BIN" "$@" >"$name.out" 2>"$name.err" &
    local wpid=$!   # last process of the pipeline = jt9_decode
    if wait_for "$name.out" "cycle_num=$need " 90; then
        ok "$name: reached decode cycle $need"
    else
        bad "$name: no decode cycle $need within 90s"; tail -5 "$name.err"
    fi
    kill -TERM "$wpid" 2>/dev/null
    local waited=0
    while kill -0 "$wpid" 2>/dev/null && [ $waited -lt 50 ]; do sleep 0.1; waited=$((waited + 1)); done
    if kill -0 "$wpid" 2>/dev/null; then
        bad "$name: did not exit within 5s of SIGTERM"; kill -9 "$wpid"
    else
        ok "$name: clean exit on SIGTERM"
    fi
    # Kill the input feeder first: bash waits for the whole pipeline
    pkill -f "sleep.3017" 2>/dev/null
    wait "$wpid" 2>/dev/null
    local rc=$?
    [ $rc = 0 ] && ok "$name: exit code 0" || bad "$name: exit code $rc"
    grep -q '^<DecodeStats> cycle_num=1 duration_s=[0-9.]* num_decodes=[0-9]* skipped_cycles=0 </DecodeStats>$' "$name.out" \
        && ok "$name: DecodeStats format" || bad "$name: DecodeStats format"
    check_clean "$name"
}

# Feed the 15s FT8 recording and keep stdin open (like a live source)
stream_case stream_ft8 1 "cat ft8.raw; sleep 3017" -m FT8 -s -j "$JT9"
first_cycle=$(awk '/^<DecodeStats> cycle_num=1 /{exit} {print}' stream_ft8.out > stream_ft8.c1; echo stream_ft8.c1)
expect_golden ft8_stream "$first_cycle"
grep -q 'num_decodes=10 ' stream_ft8.out && ok "stream_ft8: jt9 reported 10 decodes" || bad "stream_ft8: jt9 decode count"

# stdin closed after the data (EOF): must keep decoding the buffered audio
stream_case stream_ft8_eof 1 "cat ft8.raw" -m FT8 -s -j "$JT9"

# Odd-sized writes that split samples across reads
stream_case stream_ft8_odd_chunks 1 "python3 -c \"
import sys,time,random
d=open('ft8.raw','rb').read(); i=0; o=sys.stdout.buffer
while i<len(d):
    n=random.choice([1,3,7,333,4097]); o.write(d[i:i+n]); o.flush(); i+=n
time.sleep(300)\"" -m FT8 -s -j "$JT9"
first_cycle=$(awk '/^<DecodeStats> cycle_num=1 /{exit} {print}' stream_ft8_odd_chunks.out > odd.c1; echo odd.c1)
expect_golden ft8_stream "$first_cycle"

stream_case stream_ft4 2 "cat ft8.raw; sleep 3017" -m FT4 -s -j "$JT9"
stream_case stream_ft2 3 "cat ft8.raw; sleep 3017" -m FT2 -s -j "$JT9"

# Cycle boundaries must be UTC aligned (FT2: multiples of 3.75s)
awk '/Triggering decode #/{sub(/s$/,"",$6); sub(/^\+/,"",$6); print $6}' stream_ft2.err | while read -r s; do
    python3 -c "import sys;s=float(sys.argv[1]);r=(s*1000)%3750;sys.exit(0 if min(r,3750-r)<300 else 1)" "$s" || echo "misaligned $s"
done >ft2_align.txt
[ -s ft2_align.txt ] && bad "stream_ft2: $(cat ft2_align.txt | head -3)" || ok "stream_ft2: decodes triggered on 3.75s UTC boundaries"

# jt9 dies mid-stream: wrapper must notice, report and exit
echo "jt9 crash in stream mode"
(cat ft8.raw; sleep 3017) | "$BIN" -m FT2 -s -j "$JT9" >crash.out 2>crash.err &
wpid=$!
wait_for crash.out "cycle_num=1 " 30
pkill -9 -f -- "-s JT9DECODE_${wpid}_"
for _ in $(seq 50); do kill -0 "$wpid" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$wpid" 2>/dev/null; then bad "crash: wrapper still running after jt9 died"; kill -9 "$wpid"; else ok "crash: wrapper exited"; fi
grep -q 'jt9 process exited unexpectedly (code: 9)' crash.err && ok "crash: reported" || { bad "crash: not reported"; tail -3 crash.err; }
pkill -f "sleep.3017" 2>/dev/null; wait "$wpid" 2>/dev/null
check_clean crash

echo
echo "$pass passed, $fail failed"
[ $fail = 0 ]
