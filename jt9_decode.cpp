/*
 * JT9 Decoder Wrapper - Standalone FT2/FT4/FT8 decoder using jt9
 *
 * A command-line wrapper for the WSJT-X jt9 decoder engine that supports:
 * - FT2, FT4, and FT8 digital modes
 * - WAV file decoding
 * - Continuous streaming from stdin (PCM audio)
 * - Mode-specific cycle timing with UTC alignment
 *
 * Uses a Qt-compatible System V shared memory implementation
 * (qt_shm_compat.h) for IPC with jt9, implementing the same shared memory
 * protocol as WSJT-X without depending on Qt.
 *
 * ARCHITECTURE: Asynchronous event-driven model matching WSJT-X
 * - poll() loop over jt9 output, cycle timer and watchdog deadlines
 * - Non-blocking decode processing
 * - Proper state management and acknowledgment
 *
 * Outputs diagnostic messages to stderr, decoded messages to stdout
 */

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "qt_shm_compat.h"

extern "C" {
#include "commons.h"
}

typedef qtcompat::SharedMemory SharedMemory;

// Set by build.sh from the VERSION file
#ifndef JT9_DECODE_VERSION
#define JT9_DECODE_VERSION "dev"
#endif

// Self-pipe used to turn SIGINT/SIGTERM into an event for the poll() loops
static int signal_pipe[2] = {-1, -1};
static volatile sig_atomic_t got_signal = 0;

static void signal_handler(int sig) {
    got_signal = sig;
    if (signal_pipe[1] != -1) {
        char c = 1;
        ssize_t r = write(signal_pipe[1], &c, 1);
        (void)r;
    }
}

static int64_t getUtcMs() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void sleep_ms(int64_t ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR && !got_signal) {}
}

static std::string trimmed(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b])) b++;
    while (e > b && isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

// Lines that are actual decodes go to stdout (same rule as WSJT-X output parsing)
static bool is_decode_line(const std::string &line) {
    return line.length() > 6 && isdigit((unsigned char)line[0]) && line[0] != '<';
}

// Equivalent of QString::toInt(): 0 if the whole string isn't a valid integer
static int to_int(const char *s) {
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    while (*end && isspace((unsigned char)*end)) end++;
    if (end == s || *end || errno || v < INT_MIN || v > INT_MAX) return 0;
    return (int)v;
}

static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Read WAV file
int read_wav_file(const std::string &filename, short *audio_data, int max_samples) {
    FILE *file = fopen(filename.c_str(), "rb");
    if (!file) {
        fprintf(stderr, "Error: Cannot open file %s\n", filename.c_str());
        fflush(stderr);
        return -1;
    }

    // Read RIFF header + start of fmt chunk (short reads leave zeros, like QDataStream)
    unsigned char hdr[36];
    memset(hdr, 0, sizeof(hdr));
    size_t got = fread(hdr, 1, sizeof(hdr), file);

    if (got < 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "Error: Not a valid WAV file\n");
        fflush(stderr);
        fclose(file);
        return -1;
    }

    // fmt chunk
    uint32_t fmt_size = le32(hdr + 16);
    uint16_t num_channels = le16(hdr + 22);
    uint32_t sample_rate = le32(hdr + 24);
    uint16_t bits_per_sample = le16(hdr + 34);

    // Skip any extra fmt bytes
    if (fmt_size > 16) {
        fseek(file, fmt_size - 16, SEEK_CUR);
    }

    // Search for data chunk
    unsigned char chunk_hdr[8];
    uint32_t data_size = 0;
    bool found_data = false;

    while (!found_data && fread(chunk_hdr, 1, 4, file) == 4) {
        memset(chunk_hdr + 4, 0, 4);
        if (fread(chunk_hdr + 4, 1, 4, file) != 4) {}
        uint32_t chunk_size = le32(chunk_hdr + 4);

        if (memcmp(chunk_hdr, "data", 4) == 0) {
            data_size = chunk_size;
            found_data = true;
            fprintf(stderr, "Found data chunk, size: %u bytes\n", data_size);
        } else {
            fprintf(stderr, "Skipping chunk \"%.4s\" (%u bytes)\n", (const char *)chunk_hdr, chunk_size);
            fseek(file, chunk_size, SEEK_CUR);
        }
    }

    if (!found_data) {
        fprintf(stderr, "Could not find data chunk in WAV file\n");
        fflush(stderr);
        fclose(file);
        return -1;
    }

    fprintf(stderr, "WAV file info:\n");
    fprintf(stderr, "  Sample rate: %u Hz\n", sample_rate);
    fprintf(stderr, "  Channels: %u\n", num_channels);
    fprintf(stderr, "  Bits per sample: %u\n", bits_per_sample);
    fprintf(stderr, "  Data size: %u bytes\n", data_size);

    int samples_to_read = data_size / sizeof(short);
    if (num_channels == 2) {
        samples_to_read /= 2;  // Stereo
    }
    if (samples_to_read > max_samples) {
        samples_to_read = max_samples;
    }

    int result;
    if (num_channels == 1) {
        // Mono
        result = (int)(fread(audio_data, 1, samples_to_read * sizeof(short), file) / sizeof(short));
        fprintf(stderr, "  Read %d samples\n", result);
    } else {
        // Stereo - take left channel
        short stereo_buf[2];
        int i;
        for (i = 0; i < samples_to_read; i++) {
            if (fread(stereo_buf, 1, 4, file) != 4) break;
            audio_data[i] = stereo_buf[0];
        }
        result = i;
        fprintf(stderr, "  Read %d samples (stereo -> mono)\n", result);
    }
    fflush(stderr);
    fclose(file);
    return result;
}

// Mode configuration structure
struct ModeConfig {
    int mode_code;      // jt9 mode code
    int cycle_ms;       // cycle time in milliseconds
    int ihsym;          // number of symbols
    int nzhsym;         // hsymStop - symbols to decode (WSJT-X m_hsymStop)
    const char* name;   // mode name
};

// Mode configurations (matching WSJT-X lines 2207, 2211, 2213)
const ModeConfig MODE_FT2  = {52, 3750, 105, 21, "FT2"};   // 3.75 seconds, hsymStop=21
const ModeConfig MODE_FT4  = {5,  7500, 105, 21, "FT4"};   // 7.5 seconds, hsymStop=21
const ModeConfig MODE_FT8  = {8,  15000, 50, 50, "FT8"};   // 15 seconds, hsymStop=50

// jt9 child process with stdout+stderr merged into one pipe
class Jt9Process {
public:
    ~Jt9Process() {
        if (pid > 0 && !finished) {
            kill(pid, SIGKILL);
            waitFinished(-1);
        }
        if (out_fd != -1) close(out_fd);
    }

    bool start(const std::string &program, const std::vector<std::string> &args, std::string &err) {
        int out_pipe[2], exec_pipe[2];
        if (pipe2(out_pipe, O_CLOEXEC) == -1 || pipe2(exec_pipe, O_CLOEXEC) == -1) {
            err = strerror(errno);
            return false;
        }

        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(program.c_str()));
        for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);

        pid = fork();
        if (pid == -1) {
            err = strerror(errno);
            return false;
        }
        if (pid == 0) {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull != -1) dup2(devnull, STDIN_FILENO);
            dup2(out_pipe[1], STDOUT_FILENO);
            dup2(out_pipe[1], STDERR_FILENO);
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGPIPE, SIG_DFL);
            // Like QProcess: a bare name is searched in PATH, then used as-is
            if (program.find('/') == std::string::npos) execvp(program.c_str(), argv.data());
            execv(program.c_str(), argv.data());
            int e = errno;
            ssize_t r = write(exec_pipe[1], &e, sizeof(e));
            (void)r;
            _exit(127);
        }

        close(out_pipe[1]);
        close(exec_pipe[1]);
        out_fd = out_pipe[0];

        int child_errno = 0;
        ssize_t n;
        do { n = read(exec_pipe[0], &child_errno, sizeof(child_errno)); } while (n == -1 && errno == EINTR);
        close(exec_pipe[0]);
        if (n > 0) {
            err = std::string("execvp: ") + strerror(child_errno);  // same wording as QProcess
            waitFinished(-1);
            return false;
        }
        return true;
    }

    // Read whatever is available; returns false on EOF
    bool readAvailable() {
        char buf[4096];
        ssize_t n = read(out_fd, buf, sizeof(buf));
        if (n > 0) {
            buffer.append(buf, n);
            return true;
        }
        if (n == -1 && (errno == EINTR || errno == EAGAIN)) return true;
        eof = true;
        return false;
    }

    bool readLine(std::string &line) {
        size_t nl = buffer.find('\n');
        if (nl == std::string::npos) {
            // Don't let output without newlines grow memory without bound
            if (buffer.size() < MAX_LINE) return false;
            nl = MAX_LINE - 1;
        }
        line = buffer.substr(0, nl + 1);
        buffer.erase(0, nl + 1);
        return true;
    }

    bool isRunning() {
        reap(false);
        return !finished;
    }

    // Wait up to timeout_ms (-1 = forever) for exit, draining output meanwhile
    bool waitFinished(int64_t timeout_ms) {
        int64_t deadline = getUtcMs() + timeout_ms;
        while (!reap(false)) {
            int64_t remaining = timeout_ms < 0 ? 100 : std::min<int64_t>(100, deadline - getUtcMs());
            if (timeout_ms >= 0 && remaining <= 0) return false;
            if (!eof && out_fd != -1) {
                struct pollfd p = {out_fd, POLLIN, 0};
                if (poll(&p, 1, (int)remaining) > 0) readAvailable();
            } else {
                sleep_ms(std::min<int64_t>(remaining, 10));
            }
        }
        while (!eof && out_fd != -1) {
            struct pollfd p = {out_fd, POLLIN, 0};
            if (poll(&p, 1, 0) <= 0 || !readAvailable()) break;
        }
        return true;
    }

    void killProcess() {
        if (pid > 0 && !finished) kill(pid, SIGKILL);
    }

    int exitCode() const {
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return WTERMSIG(status);
        return 0;
    }

    static const size_t MAX_LINE = 1 << 20;

    pid_t pid = -1;
    int out_fd = -1;
    bool eof = false;
    std::string buffer;

private:
    bool reap(bool block) {
        if (finished || pid <= 0) return true;
        pid_t r;
        do { r = waitpid(pid, &status, block ? 0 : WNOHANG); } while (r == -1 && errno == EINTR);
        if (r == pid || (r == -1 && errno == ECHILD)) finished = true;
        return finished;
    }

    bool finished = false;
    int status = 0;
};

// Audio reader thread - continuously reads samples from stdin
class AudioReader {
public:
    AudioReader(short *buffer, int buffer_size, std::mutex *mutex)
        : circ_buffer(buffer), buffer_size(buffer_size), buffer_mutex(mutex),
          write_pos(0), total_samples(0), should_stop(false) {}

    ~AudioReader() {
        stop();
        if (thread.joinable()) thread.join();
    }

    void start() { thread = std::thread(&AudioReader::run, this); }
    void stop() { should_stop = true; }
    int64_t getTotalSamples() { return total_samples.load(); }
    int getWritePos() { return write_pos.load(); }

private:
    void run() {
        unsigned char raw[8192 + 1];
        size_t carry = 0;  // odd trailing byte from the previous read

        while (!should_stop) {
            // poll with a timeout so stop() is honoured even if stdin is idle
            struct pollfd p = {STDIN_FILENO, POLLIN, 0};
            int pr = poll(&p, 1, 200);
            if (pr == 0) continue;
            if (pr == -1) {
                if (errno == EINTR) continue;
                break;
            }

            ssize_t n = read(STDIN_FILENO, raw + carry, sizeof(raw) - 1 - carry);
            if (n == 0) break;  // EOF
            if (n == -1) {
                if (errno == EINTR || errno == EAGAIN) continue;
                break;
            }

            size_t bytes = carry + n;
            size_t samples_read = bytes / sizeof(short);

            // Lock and copy samples to circular buffer
            buffer_mutex->lock();
            for (size_t i = 0; i < samples_read; i++) {
                short s;
                memcpy(&s, raw + i * sizeof(short), sizeof(short));
                circ_buffer[write_pos] = s;
                write_pos = (write_pos + 1) % buffer_size;
                total_samples++;
            }
            buffer_mutex->unlock();

            carry = bytes % sizeof(short);
            if (carry) raw[0] = raw[bytes - 1];
        }
    }

    short *circ_buffer;
    int buffer_size;
    std::mutex *buffer_mutex;
    std::atomic<int> write_pos;
    std::atomic<int64_t> total_samples;
    std::atomic<bool> should_stop;
    std::thread thread;
};

// Asynchronous stream decoder - matches WSJT-X architecture
class StreamDecoder {
public:
    StreamDecoder(SharedMemory *shm, dec_data_t *dec, Jt9Process *jt9_proc, const ModeConfig &mode_cfg)
        : sharedMemory(shm), dec_data(dec), jt9(jt9_proc), mode(mode_cfg),
          decode_in_progress(false), total_decodes(0), skipped_cycles(0),
          jt9_decode_count(0), watchdog_fires(0), decode_start_ms(0)
    {
        SAMPLES_PER_CYCLE = (RX_SAMPLE_RATE * mode.cycle_ms) / 1000;
        BUFFER_SIZE = NTMAX * RX_SAMPLE_RATE;

        // Allocate circular buffer
        circ_buffer = new short[BUFFER_SIZE];

        // Start audio reader thread
        reader = new AudioReader(circ_buffer, BUFFER_SIZE, &buffer_mutex);
        reader->start();

        fprintf(stderr, "Stream mode: Reading 12kHz 16-bit mono PCM from stdin\n");
        fprintf(stderr, "%s cycle time: %d ms (%d samples)\n", mode.name, mode.cycle_ms, SAMPLES_PER_CYCLE);
        fprintf(stderr, "Triggering decodes at UTC-aligned %g second boundaries\n", mode.cycle_ms / 1000.0);
        fflush(stderr);
    }

    ~StreamDecoder() {
        delete reader;
        delete[] circ_buffer;
    }

    // Event loop: returns the process exit code
    int run() {
        fprintf(stderr, "Waiting for first cycle boundary...\n");
        fflush(stderr);

        int64_t next_cycle_ms = -1;     // -1 until enough samples have arrived
        int64_t watchdog_ms = -1;       // -1 when watchdog not armed

        while (!quit) {
            int64_t now = getUtcMs();

            if (next_cycle_ms < 0) {
                // Wait for enough samples to accumulate
                if (reader->getTotalSamples() >= SAMPLES_PER_CYCLE) {
                    // Calculate time to next cycle boundary
                    int64_t wait_ms = msToNextCycle();
                    if (wait_ms > 100) {
                        fprintf(stderr, "Waiting %lld ms for cycle boundary...\n", (long long)wait_ms);
                        fflush(stderr);
                        next_cycle_ms = now + wait_ms;
                    } else {
                        next_cycle_ms = now;
                    }
                    first_cycle = true;
                }
            }

            if (next_cycle_ms >= 0 && now >= next_cycle_ms) {
                if (first_cycle) {
                    fprintf(stderr, "Starting decode loop...\n");
                    fflush(stderr);
                    first_cycle = false;
                }
                // Next boundary is computed from the schedule, so timing doesn't drift
                next_cycle_ms += mode.cycle_ms;
                if (next_cycle_ms <= now) next_cycle_ms = now + msToNextCycle();
                onCycleTimer(watchdog_ms);
                continue;
            }

            if (watchdog_ms >= 0 && now >= watchdog_ms) {
                watchdog_ms = -1;
                onDecodeWatchdog();
                continue;
            }

            // Sleep until the next deadline, jt9 output or a signal
            int64_t timeout = next_cycle_ms < 0 ? 100 : next_cycle_ms - now;
            if (watchdog_ms >= 0) timeout = std::min(timeout, watchdog_ms - now);
            timeout = std::max<int64_t>(0, std::min<int64_t>(timeout, 1000));

            struct pollfd fds[2] = {{jt9->out_fd, POLLIN, 0}, {signal_pipe[0], POLLIN, 0}};
            int pr = poll(fds, 2, (int)timeout);
            if (pr == -1 && errno != EINTR) break;

            if (got_signal) {
                fprintf(stderr, "Received signal %d, shutting down\n", (int)got_signal);
                fflush(stderr);
                break;
            }

            if (pr > 0 && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
                bool open = jt9->readAvailable();
                readFromStdout(watchdog_ms);
                if (!open) {
                    // Output closed: jt9 has exited (or is about to)
                    jt9->waitFinished(1000);
                    jt9Finished();
                }
            }
        }
        return 0;
    }

private:
    // Called when jt9 has output ready (WSJT-X style: readFromStdout)
    void readFromStdout(int64_t &watchdog_ms) {
        std::string raw;
        while (jt9->readLine(raw)) {
            std::string line = trimmed(raw);

            // Check for decode finished marker (matching WSJT-X line 6233)
            if (line.find("<DecodeFinished>") != std::string::npos) {
                // Extract decode count from <DecodeFinished> line
                // Format: "<DecodeFinished>   nsynced  ndecoded  navg"
                // We want the second number (ndecoded)
                int nsynced, ndecoded;
                if (line.size() > 16 && sscanf(line.c_str() + 16, "%d %d", &nsynced, &ndecoded) == 2) {
                    jt9_decode_count = ndecoded;
                }
                watchdog_ms = -1;
                decodeDone();  // Call decodeDone like WSJT-X does (line 6244)
            } else if (is_decode_line(line)) {
                // Actual decode line - output to stdout
                printf("%s\n", line.c_str());
                fflush(stdout);
                checkStdout();
            } else if (!line.empty()) {
                // Debug/diagnostic output
                fprintf(stderr, "jt9: %s\n", line.c_str());
                fflush(stderr);
            }
        }
    }

    // Called at each cycle boundary
    void onCycleTimer(int64_t &watchdog_ms) {
        // Check if jt9 is still running
        if (!jt9->isRunning()) {
            fprintf(stderr, "Error: jt9 process is not running!\n");
            fflush(stderr);
            quit = true;
            return;
        }

        // Skip this cycle if previous decode still running (matching WSJT-X behavior)
        if (decode_in_progress) {
            skipped_cycles++;
            fprintf(stderr, "Warning: Previous decode still running, skipping this cycle (total skipped: %d)\n",
                    skipped_cycles);
            fflush(stderr);
            return;
        }

        // Check if we have enough samples
        if (reader->getTotalSamples() < SAMPLES_PER_CYCLE) {
            fprintf(stderr, "Warning: Not enough samples yet (%lld < %d)\n",
                    (long long)reader->getTotalSamples(), SAMPLES_PER_CYCLE);
            fflush(stderr);
            return;
        }

        // Mark decode as in progress and arm watchdog (2 full cycles as timeout)
        decode_in_progress = true;
        decode_start_ms = getUtcMs();
        watchdog_ms = decode_start_ms + mode.cycle_ms * 2;

        // Get current UTC time
        time_t now = time(NULL);
        struct tm tm_info;
        gmtime_r(&now, &tm_info);
        int nutc = tm_info.tm_hour * 100 + tm_info.tm_min;

        // Calculate precise time for logging
        int64_t utc_ms = getUtcMs();
        int64_t ms_in_minute = utc_ms % 60000;
        double seconds_in_minute = ms_in_minute / 1000.0;

        total_decodes++;
        fprintf(stderr, "Triggering decode #%d at %04d +%.3fs (%d samples)\n",
                total_decodes, nutc, seconds_in_minute, SAMPLES_PER_CYCLE);
        fflush(stderr);

        // Copy audio samples directly to shared memory (matching original working version)
        // No shared memory lock needed for audio data - only buffer_mutex
        buffer_mutex.lock();
        int write_pos = reader->getWritePos();
        int read_start = (write_pos - SAMPLES_PER_CYCLE + BUFFER_SIZE) % BUFFER_SIZE;

        if (read_start + SAMPLES_PER_CYCLE <= BUFFER_SIZE) {
            // Contiguous block - copy directly to shared memory
            memcpy(dec_data->d2, circ_buffer + read_start, SAMPLES_PER_CYCLE * sizeof(short));
        } else {
            // Wraps around - copy in two parts directly to shared memory
            int first_part = BUFFER_SIZE - read_start;
            memcpy(dec_data->d2, circ_buffer + read_start, first_part * sizeof(short));
            memcpy(dec_data->d2 + first_part, circ_buffer, (SAMPLES_PER_CYCLE - first_part) * sizeof(short));
        }
        buffer_mutex.unlock();

        // Lock shared memory to set params and trigger decode atomically
        sharedMemory->lock();
        dec_data->params.nutc = nutc;
        dec_data->params.kin = SAMPLES_PER_CYCLE;
        dec_data->params.newdat = true;
        dec_data->ipc[0] = mode.ihsym;
        dec_data->ipc[1] = 1;   // start decode
        dec_data->ipc[2] = -1;  // not done
        sharedMemory->unlock();

        // RETURN IMMEDIATELY - don't wait! (matching WSJT-X line 5651)
        // jt9 output is picked up by the event loop when done
    }

    // Called if jt9 never sends <DecodeFinished> within 2 cycle periods
    void onDecodeWatchdog() {
        watchdog_fires++;
        fprintf(stderr, "Warning: Decode watchdog fired (total: %d) - jt9 did not finish in time, resetting state\n",
                watchdog_fires);
        fflush(stderr);

        // Reset flag so the next cycle can trigger a fresh decode
        decode_in_progress = false;
        jt9_decode_count = 0;

        // Force-acknowledge so jt9 is not left waiting for ipc[2]
        sharedMemory->lock();
        dec_data->ipc[2] = 1;
        sharedMemory->unlock();
    }

    // Called when decode is complete (matching WSJT-X decodeDone at line 5717)
    void decodeDone() {
        // Calculate decode duration
        int64_t decode_end_ms = getUtcMs();
        double decode_duration_s = (decode_end_ms - decode_start_ms) / 1000.0;

        // Output machine-readable statistics to stdout
        printf("<DecodeStats> cycle_num=%d duration_s=%.3f num_decodes=%d skipped_cycles=%d </DecodeStats>\n",
               total_decodes, decode_duration_s, jt9_decode_count, skipped_cycles);
        fflush(stdout);
        checkStdout();

        // Clear decode in progress flag BEFORE acknowledgment (matching WSJT-X line 5750)
        decode_in_progress = false;
        jt9_decode_count = 0;

        // Acknowledge decode (matching WSJT-X: to_jt9(m_ihsym, -1, 1) at line 5756)
        sharedMemory->lock();
        dec_data->ipc[2] = 1;  // Tell jt9 we know it has finished
        sharedMemory->unlock();
    }

    // Whoever reads our stdout has gone away: shut down cleanly
    void checkStdout() {
        if (!ferror(stdout) || quit) return;
        fprintf(stderr, "Error: cannot write to stdout (%s), shutting down\n", strerror(errno));
        fflush(stderr);
        quit = true;
    }

    void jt9Finished() {
        fprintf(stderr, "Error: jt9 process exited unexpectedly (code: %d)\n", jt9->exitCode());
        fflush(stderr);
        quit = true;
    }

    int64_t msToNextCycle() {
        int64_t now_ms = getUtcMs();
        int64_t ms_in_cycle = now_ms % mode.cycle_ms;
        return mode.cycle_ms - ms_in_cycle;
    }

    SharedMemory *sharedMemory;
    dec_data_t *dec_data;
    Jt9Process *jt9;
    ModeConfig mode;

    short *circ_buffer;
    int BUFFER_SIZE;
    int SAMPLES_PER_CYCLE;
    std::mutex buffer_mutex;
    AudioReader *reader;

    bool decode_in_progress;
    int total_decodes;
    int skipped_cycles;
    int jt9_decode_count;
    int watchdog_fires;
    int64_t decode_start_ms;
    bool first_cycle = false;
    bool quit = false;
};

// Removes the per-instance temp directory on every exit path
struct TempDir {
    std::string path;
    bool owned = false;

    ~TempDir() {
        if (!owned) return;
        if (remove_recursively(path)) {
            fprintf(stderr, "Cleaned up temp directory: %s\n", path.c_str());
        } else {
            fprintf(stderr, "Warning: Could not remove temp directory: %s\n", path.c_str());
        }
    }

    static bool remove_recursively(const std::string &dir) {
        DIR *d = opendir(dir.c_str());
        if (!d) return errno == ENOENT;
        bool ok = true;
        while (struct dirent *e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            std::string p = dir + "/" + e->d_name;
            struct stat st;
            if (lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) ok = remove_recursively(p) && ok;
            else ok = (unlink(p.c_str()) == 0) && ok;
        }
        closedir(d);
        return (rmdir(dir.c_str()) == 0) && ok;
    }
};

static void usage_hint(const char *argv0) {
    fprintf(stderr, "Usage: %s -j <jt9_path> [options] [<wav_file>|-s]\n", argv0);
    fprintf(stderr, "Use --help for more information\n");
}

int main(int argc, char *argv[]) {
    // Create unique application name for this instance (for shared memory key)
    const long pid = (long)getpid();
    std::string unique_app_name = "JT9DECODE_" + std::to_string(pid) + "_" + std::to_string(getUtcMs());

    // Parse command-line arguments
    std::string wav_file;
    int depth = 3;               // Decoding depth 1-3
    int freq_low = 100;          // Low frequency Hz (matching WSJT-X typical range)
    int freq_high = 3000;        // High frequency Hz (matching WSJT-X typical range)
    std::string jt9_path;
    bool stream_mode = false;    // Stream PCM from stdin
    bool multithread = false;    // Multithreaded FT8 decoding
    std::string mode_str = "FT2";    // Default mode
    const ModeConfig *mode = &MODE_FT2;  // Default to FT2

    // Simple argument parser
    int i = 1;
    while (i < argc) {
        std::string arg = argv[i];

        if (arg == "-d" && i + 1 < argc) {
            depth = to_int(argv[++i]);
        } else if (arg == "-j" && i + 1 < argc) {
            jt9_path = argv[++i];
        } else if (arg == "-m" && i + 1 < argc) {
            mode_str = argv[++i];
            std::transform(mode_str.begin(), mode_str.end(), mode_str.begin(), ::toupper);
            if (mode_str == "FT2") {
                mode = &MODE_FT2;
            } else if (mode_str == "FT4") {
                mode = &MODE_FT4;
            } else if (mode_str == "FT8") {
                mode = &MODE_FT8;
            } else {
                fprintf(stderr, "Error: Unknown mode '%s'. Valid modes: FT2, FT4, FT8\n", mode_str.c_str());
                return 1;
            }
        } else if (arg == "-s") {
            stream_mode = true;
        } else if (arg == "-t" || arg == "--multithread") {
            multithread = true;
        } else if (arg == "--version") {
            printf("jt9_decode %s\n", JT9_DECODE_VERSION);
            return 0;
        } else if (arg == "--help" || arg == "-help") {
            const char *a0 = argv[0];
            fprintf(stderr, "Usage: %s -j <jt9_path> [options] [<wav_file>|-s]\n", a0);
            fprintf(stderr, "\n");
            fprintf(stderr, "Decode FT2/FT4/FT8 signals from WAV file or stdin stream using jt9\n");
            fprintf(stderr, "\n");
            fprintf(stderr, "Required:\n");
            fprintf(stderr, "  -j <path>     Path to jt9 binary\n");
            fprintf(stderr, "\n");
            fprintf(stderr, "Options:\n");
            fprintf(stderr, "  -m <mode>     Mode: FT2, FT4, or FT8 (default: FT2)\n");
            fprintf(stderr, "                  FT2: 3.75s cycle, 105 symbols\n");
            fprintf(stderr, "                  FT4: 7.5s cycle, 105 symbols\n");
            fprintf(stderr, "                  FT8: 15s cycle, 50 symbols\n");
            fprintf(stderr, "  -d <depth>    Decoding depth 1-3 (default: 3)\n");
            fprintf(stderr, "  -s            Stream mode: read 12kHz 16-bit mono PCM from stdin\n");
            fprintf(stderr, "                Triggers decodes at cycle boundaries aligned to UTC\n");
            fprintf(stderr, "  -t, --multithread  Enable multithreaded FT8 decoding (FT8 only)\n");
            fprintf(stderr, "                     Uses multiple CPU cores for faster decoding\n");
            fprintf(stderr, "  --version     Show version\n");
            fprintf(stderr, "  --help        Show this help message\n");
            fprintf(stderr, "\n");
            fprintf(stderr, "Examples:\n");
            fprintf(stderr, "  # Decode WAV files\n");
            fprintf(stderr, "  %s -j /usr/local/bin/jt9 recording.wav\n", a0);
            fprintf(stderr, "  %s -j /opt/jt9 -m FT8 -d 2 recording.wav\n", a0);
            fprintf(stderr, "\n");
            fprintf(stderr, "  # Stream mode (continuous decoding)\n");
            fprintf(stderr, "  rtl_fm -f 144.174M -s 12k | %s -j /usr/local/bin/jt9 -m FT2 -s\n", a0);
            fprintf(stderr, "  rtl_fm -f 14.074M -s 12k | %s -j /usr/local/bin/jt9 -m FT8 -s\n", a0);
            fprintf(stderr, "  sox input.wav -t raw -r 12000 -e signed -b 16 -c 1 - | %s -j jt9 -m FT4 -s\n", a0);
            return 0;
        } else if (arg.empty() || arg[0] != '-') {
            wav_file = arg;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            fprintf(stderr, "Use --help for usage information\n");
            return 1;
        }
        i++;
    }

    if (!stream_mode && wav_file.empty()) {
        fprintf(stderr, "Error: No WAV file specified (use -s for stream mode)\n");
        usage_hint(argv[0]);
        return 1;
    }

    if (stream_mode && !wav_file.empty()) {
        fprintf(stderr, "Error: Cannot specify both -s (stream mode) and WAV file\n");
        return 1;
    }

    if (jt9_path.empty()) {
        fprintf(stderr, "Error: jt9 path not specified\n");
        usage_hint(argv[0]);
        return 1;
    }

    // Turn SIGINT/SIGTERM into a clean shutdown (stop jt9, free shared memory)
    if (pipe2(signal_pipe, O_CLOEXEC | O_NONBLOCK) == -1) {
        fprintf(stderr, "Failed to create signal pipe: %s\n", strerror(errno));
        return 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    // A closed stdout must not kill us before jt9 and the shared memory are cleaned up
    signal(SIGPIPE, SIG_IGN);

    // Create shared memory (Qt-compatible, so jt9 can attach with QSharedMemory)
    SharedMemory sharedMemory(unique_app_name);

    // Try to attach first (in case it exists from previous run)
    if (sharedMemory.attach()) {
        fprintf(stderr, "Detaching from existing shared memory\n");
        sharedMemory.detach();
    }

    // Create new shared memory
    if (!sharedMemory.create(sizeof(dec_data_t))) {
        fprintf(stderr, "Failed to create shared memory: %s\n", sharedMemory.errorString().c_str());
        return 1;
    }

    fprintf(stderr, "Shared memory created with key: %s\n", unique_app_name.c_str());
    fprintf(stderr, "Structure size: %zu bytes\n", sizeof(dec_data_t));
    fprintf(stderr, "\n");

    // Lock and initialize
    sharedMemory.lock();
    dec_data_t *dec_data = static_cast<dec_data_t*>(sharedMemory.data());
    memset(dec_data, 0, sizeof(dec_data_t));

    // Set up common parameters for decoding (matching WSJT-X lines 5430-5490)
    dec_data->params.nmode = mode->mode_code;  // Mode code (52=FT2, 5=FT4, 8=FT8)
    dec_data->params.ntrperiod = mode->cycle_ms / 1000;  // TR period in seconds
    dec_data->params.ndepth = depth;
    dec_data->params.nfa = freq_low;
    dec_data->params.nfb = freq_high;
    dec_data->params.nfqso = 1500;
    dec_data->params.nftx = 1500;  // TX frequency (not used in RX-only mode but must be set)
    dec_data->params.ntol = 100;
    dec_data->params.nagain = false;
    dec_data->params.nQSOProgress = 0;
    dec_data->params.lapcqonly = false;  // CRITICAL: false for normal RX (true would only decode CQ messages)
    dec_data->params.nsubmode = 0;
    dec_data->params.ndiskdat = true;  // TESTING: Try true for both modes
    dec_data->params.lmultift8 = multithread;  // Enable multithreaded FT8 (FT8 only)
    dec_data->params.nzhsym = mode->nzhsym;  // hsymStop - critical for decode count (WSJT-X line 2466)

    // yymmdd for non-disk data (WSJT-X line 5396)
    if (stream_mode) {  // FIXED: was !stream_mode
        dec_data->params.yymmdd = -1;  // Critical for streaming mode
    }

    // Critical parameters for decode count (WSJT-X lines 5431-5434)
    dec_data->params.n2pass = 1;  // FIXED: Was 2, should be 1 (WSJT-X line 5431)
    dec_data->params.nranera = 10000;  // Random erasure trials (WSJT-X default)
    dec_data->params.naggressive = 0;  // Aggressive level (0=normal, WSJT-X line 5434)
    dec_data->params.nrobust = 0;  // Robust mode off (WSJT-X line 5435)

    // FT8 AP (a priori) decoding - CRITICAL for decode count (WSJT-X lines 5476-5478)
    if (mode->mode_code == 8) {  // FT8
        dec_data->params.lft8apon = true;  // Enable AP decoding
        dec_data->params.napwid = 50;  // AP bandwidth (default for HF)
    } else {
        dec_data->params.lft8apon = false;
        dec_data->params.napwid = 0;
    }

    // Set datetime (YYYYMMDD_HHMMSS format)
    time_t now_t = time(NULL);
    struct tm now_tm;
    gmtime_r(&now_t, &now_tm);
    char datetime_str[20];
    strftime(datetime_str, sizeof(datetime_str), "%Y%m%d_%H%M%S", &now_tm);
    strncpy(dec_data->params.datetime, datetime_str, 20);

    strncpy(dec_data->params.mycall, "K1ABC", 12);
    strncpy(dec_data->params.mygrid, "FN20", 6);
    strncpy(dec_data->params.hiscall, "", 12);  // Empty for RX-only
    strncpy(dec_data->params.hisgrid, "", 6);   // Empty for RX-only

    sharedMemory.unlock();

    // Create unique temporary directory in /dev/shm for this instance
    TempDir temp_dir;
    temp_dir.path = "/dev/shm/jt9_decode_" + std::to_string(pid) + "_" + std::to_string(getUtcMs());

    if (mkdir(temp_dir.path.c_str(), 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Warning: Could not create temp directory in /dev/shm, falling back to /tmp\n");
        temp_dir.path = "/tmp";
    } else {
        temp_dir.owned = true;
        fprintf(stderr, "Created temp directory: %s\n", temp_dir.path.c_str());
    }

    // Start jt9 process
    fprintf(stderr, "Starting jt9 decoder...\n");
    fflush(stderr);

    std::vector<std::string> args = {
        "-s", unique_app_name,
        "-w", "1",
        "-m", "1",
        "-e", ".",
        "-a", ".",
        "-t", temp_dir.path,
    };

    // Verify jt9 binary exists
    struct stat jt9_stat;
    if (stat(jt9_path.c_str(), &jt9_stat) != 0) {
        fprintf(stderr, "jt9 binary not found at: %s\n", jt9_path.c_str());
        return 1;
    }

    fprintf(stderr, "Using jt9 at: %s\n", jt9_path.c_str());
    fflush(stderr);

    // Capture jt9 output (stdout and stderr merged)
    Jt9Process jt9;
    std::string start_error;
    if (!jt9.start(jt9_path, args, start_error)) {
        fprintf(stderr, "Failed to start jt9: %s\n", start_error.c_str());
        return 1;
    }

    fprintf(stderr, "\nDecoder parameters:\n");
    fprintf(stderr, "  Mode: %s (%d)\n", mode->name, mode->mode_code);
    fprintf(stderr, "  Cycle time: %g seconds\n", mode->cycle_ms / 1000.0);
    fprintf(stderr, "  Depth: %d\n", depth);
    fprintf(stderr, "  Frequency range: %d - %d Hz\n", freq_low, freq_high);
    if (multithread && mode->mode_code == 8) {
        fprintf(stderr, "  Multithreaded: enabled (FT8)\n");
    }
    fflush(stderr);

    int result = 0;

    if (stream_mode) {
        // Streaming mode: asynchronous event-driven processing (WSJT-X style)
        {
            StreamDecoder decoder(&sharedMemory, dec_data, &jt9, *mode);
            result = decoder.run();
        }

        // Cleanup: terminate jt9
        fprintf(stderr, "Terminating jt9...\n");
        sharedMemory.lock();
        dec_data->ipc[1] = 999;
        sharedMemory.unlock();

        if (!jt9.waitFinished(2000)) {
            jt9.killProcess();
            jt9.waitFinished(-1);
        }
    } else {
        // WAV file mode: read file and decode once
        sharedMemory.lock();

        fprintf(stderr, "\nReading WAV file: %s\n", wav_file.c_str());
        int nsamples = read_wav_file(wav_file, dec_data->d2, NTMAX*RX_SAMPLE_RATE);
        if (nsamples < 0) {
            sharedMemory.unlock();
            jt9.killProcess();
            jt9.waitFinished(-1);
            return 1;
        }

        // Set up parameters for this decode
        time_t now = time(NULL);
        struct tm tm_info;
        gmtime_r(&now, &tm_info);

        dec_data->params.nutc = tm_info.tm_hour * 100 + tm_info.tm_min;
        dec_data->params.kin = nsamples;
        dec_data->params.newdat = true;

        fprintf(stderr, "  Samples: %d\n", nsamples);
        fprintf(stderr, "  UTC: %04d\n", dec_data->params.nutc);
        fprintf(stderr, "\n");

        // Signal jt9 to start decoding
        dec_data->ipc[0] = mode->ihsym;  // ihsym (105 for FT2/FT4, 50 for FT8)
        dec_data->ipc[1] = 1;             // start decoding
        dec_data->ipc[2] = -1;            // not done

        sharedMemory.unlock();

        // Wait for jt9 to report <DecodeFinished> rather than a fixed time, so a
        // slow CPU (e.g. a Raspberry Pi) never has its decode cut short. Output is
        // drained meanwhile so jt9 never blocks. SIGINT/SIGTERM cut the wait
        // short; jt9 is still told to stop below.
        const int64_t DECODE_TIMEOUT_MS = 120000;
        int64_t decode_deadline = getUtcMs() + DECODE_TIMEOUT_MS;
        bool decode_finished = false;
        while (!got_signal && !decode_finished && getUtcMs() < decode_deadline) {
            if (jt9.waitFinished(100)) break;  // jt9 exited
            decode_finished = jt9.buffer.find("<DecodeFinished>") != std::string::npos;
        }
        if (got_signal) {
            fprintf(stderr, "Received signal %d, shutting down\n", (int)got_signal);
            result = 1;
        } else if (!decode_finished && jt9.isRunning()) {
            fprintf(stderr, "Warning: jt9 did not finish decoding within %lld s\n",
                    (long long)(DECODE_TIMEOUT_MS / 1000));
        }

        // Acknowledge decode is done
        sharedMemory.lock();
        dec_data->ipc[2] = 1;  // acknowledge
        sharedMemory.unlock();

        jt9.waitFinished(100);

        // Tell jt9 to terminate
        sharedMemory.lock();
        dec_data->ipc[1] = 999;  // terminate
        sharedMemory.unlock();

        // Wait for jt9 to finish and capture output
        if (!jt9.waitFinished(5000)) {
            fprintf(stderr, "jt9 didn't exit cleanly, killing...\n");
            fflush(stderr);
            jt9.killProcess();
            jt9.waitFinished(-1);
        }

        // Process jt9 output
        size_t start = 0;
        const std::string &output = jt9.buffer;
        while (start < output.size()) {
            size_t end = output.find('\n', start);
            if (end == std::string::npos) end = output.size();
            std::string line = output.substr(start, end - start);
            start = end + 1;
            if (line.empty()) continue;

            if (is_decode_line(line)) {
                printf("%s\n", line.c_str());
                fflush(stdout);
            } else {
                fprintf(stderr, "%s\n", line.c_str());
            }
        }

        fprintf(stderr, "jt9 finished with exit code: %d\n", jt9.exitCode());
        fflush(stderr);
    }

    return result;
}
