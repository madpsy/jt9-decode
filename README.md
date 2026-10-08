# JT9 Decoder - Standalone FT2/FT4/FT8 Decoder

A standalone command-line wrapper for the WSJT-X jt9 decoder engine, supporting FT2, FT4, and FT8 digital modes.

## Features

- **Multi-mode support**: FT2, FT4, and FT8
- Decode signals from WAV files or continuous audio streams
- **Stream mode**: Continuously decode from stdin (PCM audio stream)
- **Multithreaded FT8 decoding**: Use multiple CPU cores for faster FT8 decoding
- Configurable decoding depth (1-3)
- Clean output separation (decoded messages to stdout, diagnostics to stderr)
- Handles WAV files with metadata chunks
- Mode-specific cycle timing with UTC alignment:
  - FT2: 3.75 second cycles
  - FT4: 7.5 second cycles
  - FT8: 15 second cycles

## Requirements

### System Dependencies

- **C++ compiler** (g++, C++11) - no other libraries are needed
- **jt9 binary** from WSJT-X (must be compiled separately)
  - jt9 must be built against **Qt 5** (as WSJT-X releases are). jt9 attaches to
    our shared memory with `QSharedMemory`; jt9_decode reproduces Qt 5's System V
    key scheme without linking Qt (see `qt_shm_compat.h`). A jt9 built against
    Qt >= 6.6 configured for POSIX IPC would not be able to attach.

### Audio Format Requirements

- **For WAV files**: 12 kHz sample rate, 16-bit, mono or stereo
- **For streaming**: 12 kHz, 16-bit signed, mono PCM on stdin

## Compilation

```bash
make
```

or directly:
```bash
g++ -o jt9_decode jt9_decode.cpp -I./wsjtx -std=c++11 -O2 -pthread -lrt
```

### Release builds (amd64 + arm64)

```bash
./build.sh                    # build dist/jt9_decode_amd64 and dist/jt9_decode_arm64
./build.sh --publish          # ...and upload them to the GitHub release named in VERSION
```

Binaries are built in `ubuntu:24.04` containers (arm64 under binfmt/qemu) and
linked fully statically, so they run on any amd64/arm64 Linux. Each one is then
checked by decoding `test_ft8.wav` with the real jt9 from that architecture's
WSJT-X .deb on the 1.0.0 release. Publishing replaces only the `jt9_decode_*`
assets on the release; anything else on it is left alone. See `./build.sh --help`.

## Tests

```bash
make test           # everything below, fast tier (~5 min)
make test-full      # everything at full size, including a 2 min soak (~15 min)

make test-interop   # shared memory/lock interop against real QSharedMemory
make test-e2e       # real jt9: WAV + stream decodes vs golden output, errors,
                    # memory use, cleanup
make test-stress    # ASan/UBSan/TSan, WAV fuzzing, hostile stdin/jt9, signals,
                    # concurrency (FULL=1 adds the soak and more repetitions)
```

The tests need `jt9`, `sox` and `python3`. `test-interop` also needs the Qt 5
development package (`qtbase5-dev`) to build its reference helper; Qt is never
needed to build or run `jt9_decode` itself.

`tests/run_e2e.sh` can compare against another build of jt9_decode
(e.g. the old Qt version): `REF=/path/to/old/jt9_decode tests/run_e2e.sh ./jt9_decode`.

## Usage

### Basic Syntax

```bash
# WAV file mode
./jt9_decode -j <jt9_path> [options] <wav_file>

# Stream mode
./jt9_decode -j <jt9_path> [options] -s
```

### Required Arguments

- `-j <path>` - Path to jt9 binary (required)

### Options

- `-m <mode>` - Mode: FT2, FT4, or FT8 (default: FT2)
  - **FT2**: 3.75 second cycles, 105 symbols (2m/70cm VHF/UHF)
  - **FT4**: 7.5 second cycles, 105 symbols (contesting, fast QSOs)
  - **FT8**: 15 second cycles, 50 symbols (HF DX, most popular)
- `-d <depth>` - Decoding depth 1-3 (default: 3)
  - Depth 1: Fast decode (fewer iterations)
  - Depth 2: Normal decode
  - Depth 3: Deep decode (more iterations, finds weaker signals)
- `-s` - Stream mode: read 12kHz 16-bit mono PCM from stdin
  - Continuously processes audio
  - Triggers decodes at cycle boundaries aligned to UTC
  - Keeps jt9 running for efficiency
- `-t, --multithread` - Enable multithreaded FT8 decoding (FT8 only)
  - Uses multiple CPU cores for faster decoding
  - Can decode more simultaneous signals
  - Provides better performance on busy bands
- `--version` - Show version
- `--help` - Show help message

## Examples

### Basic Usage

Decode FT2 with default settings (depth 3):
```bash
./jt9_decode -j /usr/local/bin/jt9 recording.wav
```

Decode FT8 signal:
```bash
./jt9_decode -j /usr/local/bin/jt9 -m FT8 ft8_recording.wav
```

Decode FT4 signal:
```bash
./jt9_decode -j /usr/local/bin/jt9 -m FT4 ft4_recording.wav
```

### Specify Decoding Depth

Fast FT8 decode (depth 1):
```bash
./jt9_decode -j /usr/local/bin/jt9 -m FT8 -d 1 recording.wav
```

Deep FT2 decode (depth 3):
```bash
./jt9_decode -j /usr/local/bin/jt9 -m FT2 -d 3 recording.wav
```

### Different jt9 Locations

```bash
./jt9_decode -j ~/Downloads/jt9 -m FT8 recording.wav
./jt9_decode -j /opt/wsjtx/bin/jt9 -m FT4 recording.wav
```

### Output Redirection

Only decoded messages (no diagnostics):
```bash
./jt9_decode -j /usr/local/bin/jt9 recording.wav 2>/dev/null
```

Only diagnostics (no decoded messages):
```bash
./jt9_decode -j /usr/local/bin/jt9 recording.wav >/dev/null
```

### Piping and Filtering

Filter for CQ calls:
```bash
./jt9_decode -j /usr/local/bin/jt9 recording.wav 2>/dev/null | grep "CQ"
```

Save decoded messages to file:
```bash
./jt9_decode -j /usr/local/bin/jt9 recording.wav 2>/dev/null > decoded.txt
```

Process multiple files:
```bash
JT9=/usr/local/bin/jt9
for file in *.wav; do
    echo "Processing $file..."
    ./jt9_decode -j "$JT9" "$file" 2>/dev/null
done
```

### Streaming Mode

Stream FT2 from 2m VHF (144.174 MHz):
```bash
rtl_fm -f 144.174M -s 12k | ./jt9_decode -j /usr/local/bin/jt9 -m FT2 -s
```

Stream FT8 from 20m HF (14.074 MHz):
```bash
rtl_fm -f 14.074M -s 12k | ./jt9_decode -j /usr/local/bin/jt9 -m FT8 -s
```

Stream FT4 from 40m HF (7.047 MHz):
```bash
rtl_fm -f 7.047M -s 12k | ./jt9_decode -j /usr/local/bin/jt9 -m FT4 -s
```

Convert and stream a WAV file:
```bash
sox recording.wav -t raw -r 12000 -e signed -b 16 -c 1 - | ./jt9_decode -j /usr/local/bin/jt9 -m FT8 -s
```

Stream from audio device (ALSA):
```bash
arecord -D hw:0,0 -f S16_LE -r 12000 -c 1 -t raw | ./jt9_decode -j /usr/local/bin/jt9 -m FT8 -s
```

Stream with custom depth:
```bash
rtl_fm -f 14.074M -s 12k | ./jt9_decode -j /usr/local/bin/jt9 -m FT8 -d 3 -s
```

**How Streaming Mode Works:**
- Continuously reads 12kHz, 16-bit signed, mono PCM from stdin
- Accumulates samples in a circular buffer (mode-dependent size)
- Triggers jt9 decode at cycle boundaries aligned to UTC time:
  - FT2: every 3.75 seconds (45,000 samples)
  - FT4: every 7.5 seconds (90,000 samples)
  - FT8: every 15 seconds (180,000 samples)
- Keeps jt9 running between decodes for efficiency (no restart overhead)
- Outputs decoded messages in real-time as they are found

## Output Format

### Decoded Messages (stdout)

```
001826   4 -0.2 1470 *  CQ EA8TN IL18
```

Format: `HHMMSS SNR DT FREQ * MESSAGE`
- `HHMMSS` - Time in UTC (hours, minutes, seconds)
- `SNR` - Signal-to-noise ratio in dB
- `DT` - Time offset in seconds
- `FREQ` - Frequency in Hz
- `*` - Decode quality indicator
- `MESSAGE` - Decoded message text

### Diagnostic Messages (stderr)

```
Shared memory created with key: JT9DECODE
Reading WAV file: recording.wav
WAV file info:
  Sample rate: 12000 Hz
  Channels: 1
  Read 162642 samples
Decoder parameters:
  Mode: FT2 (52)
  Cycle time: 3.75 seconds
  Depth: 3
Using jt9 at: /home/user/Downloads/jt9
jt9 finished with exit code: 0
```

## Converting Audio Files

If you have recordings in other formats (e.g., WebM), convert to WAV first:

```bash
ffmpeg -i recording.webm -ar 12000 -ac 1 -acodec pcm_s16le recording.wav
```

Parameters:
- `-ar 12000` - Resample to 12 kHz
- `-ac 1` - Convert to mono
- `-acodec pcm_s16le` - 16-bit PCM format

## Troubleshooting

### "Error: jt9 path not specified"

You must specify the jt9 binary path with the `-j` option:
```bash
./jt9_decode -j /path/to/jt9 recording.wav
```

### "jt9 binary not found at: /path/to/jt9"

Check that the jt9 binary exists and is executable:
```bash
ls -l /path/to/jt9
which jt9
```

### "Error: Cannot open file"

Check that the WAV file exists and is readable:
```bash
ls -l recording.wav
file recording.wav
```

### No Decodes

Try increasing the depth:
```bash
./jt9_decode -d 3 recording.wav
```

Check the WAV file format:
```bash
ffprobe recording.wav
```

Ensure it's 12 kHz, 16-bit audio.

## Deployment and Portability

The `jt9_decode` binary only depends on the C/C++ runtime (libstdc++, libgcc_s,
libc). jt9 itself still needs whatever libraries it was built with (including Qt 5).

### Copying to Another PC

1. **Copy both binaries:**
   - `jt9_decode` (the wrapper)
   - `jt9` (the decoder binary from WSJT-X)

2. **Make executable:**
   ```bash
   chmod +x jt9_decode jt9
   ```

Check dependencies with:
```bash
ldd ./jt9_decode
```

## Technical Details

- Uses Qt-compatible System V shared memory for IPC with jt9 (no Qt dependency)
- SIGINT/SIGTERM shut down cleanly: jt9 is stopped and the shared memory,
  semaphore and temp directory are removed
- Implements the same shared memory protocol as WSJT-X
- Properly handles WAV files with LIST/INFO metadata chunks
- WAV mode waits for jt9 to report the decode finished (up to 120 s), so slow
  CPUs such as a Raspberry Pi never have a decode cut short
- Supports multiple modes with correct parameters:
  - **FT2**: mode code 52, 105 symbols, 3.75s cycles
  - **FT4**: mode code 5, 105 symbols, 7.5s cycles
  - **FT8**: mode code 8, 50 symbols, 15s cycles
- Configures frequency range 200-5000 Hz by default
- Streaming mode uses circular buffer with mode-specific cycle timing
- UTC-aligned decode triggers for proper timing synchronization
- Keeps jt9 process running in streaming mode for efficiency

## License

This decoder interfaces with WSJT-X's jt9 binary. Please refer to WSJT-X licensing terms.

## Author

Created for standalone FT2/FT4/FT8 signal decoding from WAV files and live audio streams.
