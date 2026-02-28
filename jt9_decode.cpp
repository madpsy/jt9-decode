/*
 * JT9 Decoder Wrapper - Standalone FT2/FT4/FT8 decoder using jt9
 *
 * A command-line wrapper for the WSJT-X jt9 decoder engine that supports:
 * - FT2, FT4, and FT8 digital modes
 * - WAV file decoding
 * - Continuous streaming from stdin (PCM audio)
 * - Mode-specific cycle timing with UTC alignment
 *
 * Uses Qt's QSharedMemory for IPC with jt9, implementing the same
 * shared memory protocol as WSJT-X.
 *
 * Outputs diagnostic messages to stderr, decoded messages to stdout
 */

#include <QCoreApplication>
#include <QSharedMemory>
#include <QProcess>
#include <QFile>
#include <QDataStream>
#include <QThread>
#include <QTextStream>
#include <QMutex>
#include <QWaitCondition>
#include <cstring>
#include <ctime>
#include <cmath>
#include <atomic>

extern "C" {
#include "commons.h"
}

// Helper to write to stderr
QTextStream qStdErr(stderr);
QTextStream qStdOut(stdout);

// Read WAV file
int read_wav_file(const QString &filename, short *audio_data, int max_samples) {
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        qStdErr << "Error: Cannot open file " << filename << "\n";
        qStdErr.flush();
        return -1;
    }
    
    QDataStream in(&file);
    in.setByteOrder(QDataStream::LittleEndian);
    
    // Read RIFF header
    char riff[4], wave[4];
    quint32 file_size;
    in.readRawData(riff, 4);
    in >> file_size;
    in.readRawData(wave, 4);
    
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(wave, "WAVE", 4) != 0) {
        qStdErr << "Error: Not a valid WAV file\n";
        qStdErr.flush();
        return -1;
    }
    
    // Read fmt chunk
    char fmt[4];
    quint32 fmt_size;
    quint16 audio_format, num_channels, bits_per_sample, block_align;
    quint32 sample_rate, byte_rate;
    
    in.readRawData(fmt, 4);
    in >> fmt_size;
    in >> audio_format;
    in >> num_channels;
    in >> sample_rate;
    in >> byte_rate;
    in >> block_align;
    in >> bits_per_sample;
    
    // Skip any extra fmt bytes
    if (fmt_size > 16) {
        file.seek(file.pos() + fmt_size - 16);
    }
    
    // Search for data chunk
    char chunk_id[4];
    quint32 chunk_size;
    quint32 data_size = 0;
    bool found_data = false;
    
    while (!found_data && in.readRawData(chunk_id, 4) == 4) {
        in >> chunk_size;
        
        if (memcmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_size;
            found_data = true;
            qStdErr << "Found data chunk, size: " << data_size << " bytes\n";
        } else {
            qStdErr << "Skipping chunk \"" << QString::fromLatin1(chunk_id, 4) << "\" (" << chunk_size << " bytes)\n";
            file.seek(file.pos() + chunk_size);
        }
    }
    
    if (!found_data) {
        qStdErr << "Could not find data chunk in WAV file\n";
        qStdErr.flush();
        return -1;
    }
    
    qStdErr << "WAV file info:\n";
    qStdErr << "  Sample rate: " << sample_rate << " Hz\n";
    qStdErr << "  Channels: " << num_channels << "\n";
    qStdErr << "  Bits per sample: " << bits_per_sample << "\n";
    qStdErr << "  Data size: " << data_size << " bytes\n";
    
    int samples_to_read = data_size / sizeof(short);
    if (num_channels == 2) {
        samples_to_read /= 2;  // Stereo
    }
    if (samples_to_read > max_samples) {
        samples_to_read = max_samples;
    }
    
    // Read audio data
    if (num_channels == 1) {
        // Mono
        int read = in.readRawData((char*)audio_data, samples_to_read * sizeof(short)) / sizeof(short);
        qStdErr << "  Read " << read << " samples\n";
        qStdErr.flush();
        return read;
    } else {
        // Stereo - take left channel
        short stereo_buf[2];
        int i;
        for (i = 0; i < samples_to_read; i++) {
            if (in.readRawData((char*)stereo_buf, 4) != 4) break;
            audio_data[i] = stereo_buf[0];
        }
        qStdErr << "  Read " << i << " samples (stereo -> mono)\n";
        qStdErr.flush();
        return i;
    }
}

// Mode configuration structure
struct ModeConfig {
    int mode_code;      // jt9 mode code
    int cycle_ms;       // cycle time in milliseconds
    int ihsym;          // number of symbols
    const char* name;   // mode name
};

// Mode configurations
const ModeConfig MODE_FT2  = {52, 3750, 105, "FT2"};   // 3.75 seconds
const ModeConfig MODE_FT4  = {5,  7500, 105, "FT4"};   // 7.5 seconds
const ModeConfig MODE_FT8  = {8,  15000, 50, "FT8"};   // 15 seconds

// Audio reader thread - continuously reads samples from stdin
class AudioReaderThread : public QThread {
public:
    AudioReaderThread(short *buffer, int buffer_size, QMutex *mutex)
        : circ_buffer(buffer), buffer_size(buffer_size), buffer_mutex(mutex),
          write_pos(0), total_samples(0), should_stop(false) {}
    
    void run() override {
        FILE *stdin_file = stdin;
        #ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        #endif
        
        short sample_buf[4096];
        
        while (!should_stop) {
            size_t samples_read = fread(sample_buf, sizeof(short), 4096, stdin_file);
            
            if (samples_read == 0) {
                if (feof(stdin_file) || ferror(stdin_file)) {
                    break;
                }
                QThread::msleep(10);
                continue;
            }
            
            // Lock and copy samples to circular buffer
            buffer_mutex->lock();
            for (size_t i = 0; i < samples_read; i++) {
                circ_buffer[write_pos] = sample_buf[i];
                write_pos = (write_pos + 1) % buffer_size;
                total_samples++;
            }
            buffer_mutex->unlock();
        }
    }
    
    void stop() { should_stop = true; }
    qint64 getTotalSamples() { return total_samples.load(); }
    int getWritePos() { return write_pos.load(); }
    
private:
    short *circ_buffer;
    int buffer_size;
    QMutex *buffer_mutex;
    std::atomic<int> write_pos;
    std::atomic<qint64> total_samples;
    std::atomic<bool> should_stop;
};

// Streaming mode: continuously read PCM from stdin and trigger decodes at cycle boundaries
int stream_mode_decode(const QString &jt9_path, int depth, int freq_low, int freq_high,
                       QSharedMemory &sharedMemory, dec_data_t *dec_data, QProcess &jt9,
                       const ModeConfig &mode) {
    
    const int SAMPLES_PER_CYCLE = (RX_SAMPLE_RATE * mode.cycle_ms) / 1000;
    const int BUFFER_SIZE = NTMAX * RX_SAMPLE_RATE;  // Total buffer size (60 seconds at 12kHz)
    const double CYCLE_SECONDS = mode.cycle_ms / 1000.0;
    
    qStdErr << "Stream mode: Reading 12kHz 16-bit mono PCM from stdin\n";
    qStdErr << mode.name << " cycle time: " << mode.cycle_ms << " ms (" << SAMPLES_PER_CYCLE << " samples)\n";
    qStdErr << "Triggering decodes at UTC-aligned " << CYCLE_SECONDS << " second boundaries\n";
    qStdErr.flush();
    
    // Circular buffer for incoming audio
    short *circ_buffer = new short[BUFFER_SIZE];
    QMutex buffer_mutex;
    
    // Decode buffer (in shared memory)
    short *decode_buffer = dec_data->d2;
    
    // Start audio reader thread
    AudioReaderThread reader_thread(circ_buffer, BUFFER_SIZE, &buffer_mutex);
    reader_thread.start();
    
    // Get current UTC time in milliseconds
    auto get_utc_ms = []() -> qint64 {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
    };
    
    // Calculate milliseconds until next cycle boundary
    auto ms_to_next_cycle = [&mode, &get_utc_ms]() -> qint64 {
        qint64 now_ms = get_utc_ms();
        qint64 ms_in_cycle = now_ms % mode.cycle_ms;
        return mode.cycle_ms - ms_in_cycle;
    };
    
    qStdErr << "Waiting for first cycle boundary...\n";
    qStdErr.flush();
    
    // Wait for enough samples to accumulate
    while (reader_thread.getTotalSamples() < SAMPLES_PER_CYCLE) {
        QThread::msleep(100);
    }
    
    // Wait for the next cycle boundary
    qint64 wait_ms = ms_to_next_cycle();
    if (wait_ms > 100) {  // Only wait if more than 100ms away
        qStdErr << "Waiting " << wait_ms << " ms for cycle boundary...\n";
        qStdErr.flush();
        QThread::msleep(wait_ms);
    }
    
    qStdErr << "Starting decode loop...\n";
    qStdErr.flush();
    
    // Main decode loop - triggers at each cycle boundary
    while (reader_thread.isRunning()) {
        // Wait for next cycle boundary
        qint64 wait_ms = ms_to_next_cycle();
        if (wait_ms > 10) {
            QThread::msleep(wait_ms);
        }
        
        // Check if we have enough samples
        if (reader_thread.getTotalSamples() < SAMPLES_PER_CYCLE) {
            QThread::msleep(100);
            continue;
        }
        
        // Lock buffer and copy samples for decoding
        buffer_mutex.lock();
        int write_pos = reader_thread.getWritePos();
        
        // Copy the last SAMPLES_PER_CYCLE samples from circular buffer
        int read_start = (write_pos - SAMPLES_PER_CYCLE + BUFFER_SIZE) % BUFFER_SIZE;
        
        if (read_start + SAMPLES_PER_CYCLE <= BUFFER_SIZE) {
            // Contiguous block
            memcpy(decode_buffer, circ_buffer + read_start, SAMPLES_PER_CYCLE * sizeof(short));
        } else {
            // Wraps around
            int first_part = BUFFER_SIZE - read_start;
            memcpy(decode_buffer, circ_buffer + read_start, first_part * sizeof(short));
            memcpy(decode_buffer + first_part, circ_buffer, (SAMPLES_PER_CYCLE - first_part) * sizeof(short));
        }
        buffer_mutex.unlock();
        
        // Get current UTC time
        time_t now = time(NULL);
        struct tm *tm_info = gmtime(&now);
        int nutc = tm_info->tm_hour * 100 + tm_info->tm_min;
        
        // Calculate precise time for logging
        qint64 utc_ms = get_utc_ms();
        qint64 ms_in_minute = utc_ms % 60000;
        double seconds_in_minute = ms_in_minute / 1000.0;
        
        qStdErr << "Triggering decode at " << QString("%1").arg(nutc, 4, 10, QChar('0'))
                << " +" << QString::number(seconds_in_minute, 'f', 3) << "s"
                << " (" << SAMPLES_PER_CYCLE << " samples)\n";
        qStdErr.flush();
        
        // Update parameters and trigger decode
        sharedMemory.lock();
        dec_data->params.nutc = nutc;
        dec_data->params.kin = SAMPLES_PER_CYCLE;
        dec_data->params.newdat = true;
        dec_data->ipc[0] = mode.ihsym;
        dec_data->ipc[1] = 1;  // start decode
        dec_data->ipc[2] = -1;  // not done
        sharedMemory.unlock();
        
        // Read jt9 output (non-blocking)
        int wait_count = 0;
        bool decode_started = false;
        while (wait_count < 100) {  // Max 10 seconds
            QThread::msleep(100);
            
            sharedMemory.lock();
            bool done = (dec_data->ipc[1] == 0);
            sharedMemory.unlock();
            
            if (done) decode_started = true;
            
            // Read any available output
            jt9.waitForReadyRead(10);
            while (jt9.canReadLine()) {
                QString line = QString::fromLocal8Bit(jt9.readLine()).trimmed();
                if (line.length() > 6 && line[0].isDigit() && !line.startsWith('<')) {
                    qStdOut << line << "\n";
                    qStdOut.flush();
                } else if (!line.isEmpty()) {
                    qStdErr << "jt9: " << line << "\n";
                    qStdErr.flush();
                }
            }
            
            if (decode_started && wait_count > 5) break;
            wait_count++;
        }
        
        // Final read
        jt9.waitForReadyRead(100);
        while (jt9.canReadLine()) {
            QString line = QString::fromLocal8Bit(jt9.readLine()).trimmed();
            if (line.length() > 6 && line[0].isDigit() && !line.startsWith('<')) {
                qStdOut << line << "\n";
                qStdOut.flush();
            } else if (!line.isEmpty()) {
                qStdErr << "jt9: " << line << "\n";
                qStdErr.flush();
            }
        }
        
        // Acknowledge decode
        sharedMemory.lock();
        dec_data->ipc[2] = 1;
        sharedMemory.unlock();
    }
    
    // Stop reader thread
    reader_thread.stop();
    reader_thread.wait();
    
    // Cleanup
    delete[] circ_buffer;
    
    // Terminate jt9
    qStdErr << "Terminating jt9...\n";
    sharedMemory.lock();
    dec_data->ipc[1] = 999;
    sharedMemory.unlock();
    
    return 0;
}

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("JT9DECODE");
    
    // Parse command-line arguments
    QString wav_file;
    int depth = 3;               // Decoding depth 1-3
    int freq_low = 200;          // Low frequency Hz
    int freq_high = 5000;        // High frequency Hz
    QString jt9_path;
    bool stream_mode = false;    // Stream PCM from stdin
    bool multithread = false;    // Multithreaded FT8 decoding
    QString mode_str = "FT2";    // Default mode
    const ModeConfig *mode = &MODE_FT2;  // Default to FT2
    
    // Simple argument parser
    int i = 1;
    while (i < argc) {
        QString arg = argv[i];
        
        if (arg == "-d" && i + 1 < argc) {
            depth = QString(argv[++i]).toInt();
        } else if (arg == "-j" && i + 1 < argc) {
            jt9_path = QString(argv[++i]);
        } else if (arg == "-m" && i + 1 < argc) {
            mode_str = QString(argv[++i]).toUpper();
            if (mode_str == "FT2") {
                mode = &MODE_FT2;
            } else if (mode_str == "FT4") {
                mode = &MODE_FT4;
            } else if (mode_str == "FT8") {
                mode = &MODE_FT8;
            } else {
                qStdErr << "Error: Unknown mode '" << mode_str << "'. Valid modes: FT2, FT4, FT8\n";
                qStdErr.flush();
                return 1;
            }
        } else if (arg == "-s") {
            stream_mode = true;
        } else if (arg == "-t" || arg == "--multithread") {
            multithread = true;
        } else if (arg == "--help" || arg == "-help") {
            qStdErr << "Usage: " << argv[0] << " -j <jt9_path> [options] [<wav_file>|-s]\n";
            qStdErr << "\n";
            qStdErr << "Decode FT2/FT4/FT8 signals from WAV file or stdin stream using jt9\n";
            qStdErr << "\n";
            qStdErr << "Required:\n";
            qStdErr << "  -j <path>     Path to jt9 binary\n";
            qStdErr << "\n";
            qStdErr << "Options:\n";
            qStdErr << "  -m <mode>     Mode: FT2, FT4, or FT8 (default: FT2)\n";
            qStdErr << "                  FT2: 3.75s cycle, 105 symbols\n";
            qStdErr << "                  FT4: 7.5s cycle, 105 symbols\n";
            qStdErr << "                  FT8: 15s cycle, 50 symbols\n";
            qStdErr << "  -d <depth>    Decoding depth 1-3 (default: 3)\n";
            qStdErr << "  -s            Stream mode: read 12kHz 16-bit mono PCM from stdin\n";
            qStdErr << "                Triggers decodes at cycle boundaries aligned to UTC\n";
            qStdErr << "  -t, --multithread  Enable multithreaded FT8 decoding (FT8 only)\n";
            qStdErr << "                     Uses multiple CPU cores for faster decoding\n";
            qStdErr << "  --help        Show this help message\n";
            qStdErr << "\n";
            qStdErr << "Examples:\n";
            qStdErr << "  # Decode WAV files\n";
            qStdErr << "  " << argv[0] << " -j /usr/local/bin/jt9 recording.wav\n";
            qStdErr << "  " << argv[0] << " -j /opt/jt9 -m FT8 -d 2 recording.wav\n";
            qStdErr << "\n";
            qStdErr << "  # Stream mode (continuous decoding)\n";
            qStdErr << "  rtl_fm -f 144.174M -s 12k | " << argv[0] << " -j /usr/local/bin/jt9 -m FT2 -s\n";
            qStdErr << "  rtl_fm -f 14.074M -s 12k | " << argv[0] << " -j /usr/local/bin/jt9 -m FT8 -s\n";
            qStdErr << "  sox input.wav -t raw -r 12000 -e signed -b 16 -c 1 - | " << argv[0] << " -j jt9 -m FT4 -s\n";
            qStdErr.flush();
            return 0;
        } else if (!arg.startsWith("-")) {
            wav_file = arg;
        } else {
            qStdErr << "Unknown option: " << arg << "\n";
            qStdErr << "Use --help for usage information\n";
            qStdErr.flush();
            return 1;
        }
        i++;
    }
    
    if (!stream_mode && wav_file.isEmpty()) {
        qStdErr << "Error: No WAV file specified (use -s for stream mode)\n";
        qStdErr << "Usage: " << argv[0] << " -j <jt9_path> [options] [<wav_file>|-s]\n";
        qStdErr << "Use --help for more information\n";
        qStdErr.flush();
        return 1;
    }
    
    if (stream_mode && !wav_file.isEmpty()) {
        qStdErr << "Error: Cannot specify both -s (stream mode) and WAV file\n";
        qStdErr.flush();
        return 1;
    }
    
    if (jt9_path.isEmpty()) {
        qStdErr << "Error: jt9 path not specified\n";
        qStdErr << "Usage: " << argv[0] << " -j <jt9_path> [options] [<wav_file>|-s]\n";
        qStdErr << "Use --help for more information\n";
        qStdErr.flush();
        return 1;
    }
    
    // Create shared memory with Qt
    QSharedMemory sharedMemory;
    sharedMemory.setKey(app.applicationName());
    
    // Try to attach first (in case it exists from previous run)
    if (sharedMemory.attach()) {
        qStdErr << "Detaching from existing shared memory\n";
        sharedMemory.detach();
    }
    
    // Create new shared memory
    if (!sharedMemory.create(sizeof(dec_data_t))) {
        qStdErr << "Failed to create shared memory: " << sharedMemory.errorString() << "\n";
        qStdErr.flush();
        return 1;
    }
    
    qStdErr << "Shared memory created with key: " << app.applicationName() << "\n";
    qStdErr << "Structure size: " << sizeof(dec_data_t) << " bytes\n";
    qStdErr << "\n";
    
    // Lock and initialize
    sharedMemory.lock();
    dec_data_t *dec_data = static_cast<dec_data_t*>(sharedMemory.data());
    memset(dec_data, 0, sizeof(dec_data_t));
    
    // Set up common parameters for decoding
    dec_data->params.nmode = mode->mode_code;  // Mode code (52=FT2, 5=FT4, 8=FT8)
    dec_data->params.ntrperiod = mode->cycle_ms / 1000;  // TR period in seconds
    dec_data->params.ndepth = depth;
    dec_data->params.nfa = freq_low;
    dec_data->params.nfb = freq_high;
    dec_data->params.nfqso = 1500;
    dec_data->params.ntol = 100;
    dec_data->params.nagain = false;
    dec_data->params.nQSOProgress = 0;
    dec_data->params.lapcqonly = false;
    dec_data->params.nsubmode = 0;
    dec_data->params.ndiskdat = !stream_mode;  // true for WAV files, false for streaming
    dec_data->params.lmultift8 = multithread;  // Enable multithreaded FT8 (FT8 only)
    
    strncpy(dec_data->params.mycall, "K1ABC", 12);
    strncpy(dec_data->params.mygrid, "FN20", 6);
    
    sharedMemory.unlock();
    
    // Start jt9 process
    qStdErr << "Starting jt9 decoder...\n";
    qStdErr.flush();
    
    QProcess jt9;
    QStringList args;
    args << "-s" << app.applicationName()
         << "-w" << "1"
         << "-m" << "1"
         << "-e" << "."
         << "-a" << "."
         << "-t" << "/tmp";
    
    // Verify jt9 binary exists
    if (!QFile::exists(jt9_path)) {
        qStdErr << "jt9 binary not found at: " << jt9_path << "\n";
        qStdErr.flush();
        return 1;
    }
    
    qStdErr << "Using jt9 at: " << jt9_path << "\n";
    qStdErr.flush();
    
    // Capture jt9 output
    jt9.setProcessChannelMode(QProcess::MergedChannels);
    jt9.start(jt9_path, args);
    
    if (!jt9.waitForStarted()) {
        qStdErr << "Failed to start jt9: " << jt9.errorString() << "\n";
        qStdErr.flush();
        return 1;
    }
    
    qStdErr << "\nDecoder parameters:\n";
    qStdErr << "  Mode: " << mode->name << " (" << mode->mode_code << ")\n";
    qStdErr << "  Cycle time: " << (mode->cycle_ms / 1000.0) << " seconds\n";
    qStdErr << "  Depth: " << depth << "\n";
    qStdErr << "  Frequency range: " << freq_low << " - " << freq_high << " Hz\n";
    if (multithread && mode->mode_code == 8) {
        qStdErr << "  Multithreaded: enabled (FT8)\n";
    }
    qStdErr.flush();
    
    int result = 0;
    
    if (stream_mode) {
        // Streaming mode: continuously read from stdin
        result = stream_mode_decode(jt9_path, depth, freq_low, freq_high, sharedMemory, dec_data, jt9, *mode);
    } else {
        // WAV file mode: read file and decode once
        sharedMemory.lock();
        dec_data = static_cast<dec_data_t*>(sharedMemory.data());
        
        qStdErr << "\nReading WAV file: " << wav_file << "\n";
        int nsamples = read_wav_file(wav_file, dec_data->d2, NTMAX*RX_SAMPLE_RATE);
        if (nsamples < 0) {
            sharedMemory.unlock();
            jt9.kill();
            jt9.waitForFinished();
            return 1;
        }
        
        // Set up parameters for this decode
        time_t now = time(NULL);
        struct tm *tm_info = gmtime(&now);
        
        dec_data->params.nutc = tm_info->tm_hour * 100 + tm_info->tm_min;
        dec_data->params.kin = nsamples;
        dec_data->params.newdat = true;
        
        qStdErr << "  Samples: " << nsamples << "\n";
        qStdErr << "  UTC: " << QString("%1").arg(dec_data->params.nutc, 4, 10, QChar('0')) << "\n";
        qStdErr << "\n";
    
        // Signal jt9 to start decoding
        dec_data->ipc[0] = mode->ihsym;  // ihsym (105 for FT2/FT4, 50 for FT8)
        dec_data->ipc[1] = 1;             // start decoding
        dec_data->ipc[2] = -1;            // not done
        
        sharedMemory.unlock();
        
        // Give jt9 time to decode
        QThread::sleep(2);
        
        // Acknowledge decode is done
        sharedMemory.lock();
        dec_data = static_cast<dec_data_t*>(sharedMemory.data());
        dec_data->ipc[2] = 1;  // acknowledge
        sharedMemory.unlock();
        
        QThread::msleep(100);
        
        // Tell jt9 to terminate
        sharedMemory.lock();
        dec_data->ipc[1] = 999;  // terminate
        sharedMemory.unlock();
        
        // Wait for jt9 to finish and capture output
        if (!jt9.waitForFinished(5000)) {
            qStdErr << "jt9 didn't exit cleanly, killing...\n";
            qStdErr.flush();
            jt9.kill();
            jt9.waitForFinished();
        }
        
        // Process jt9 output
        QString output = QString::fromLocal8Bit(jt9.readAllStandardOutput());
        QStringList lines = output.split('\n', Qt::SkipEmptyParts);
        
        for (const QString &line : lines) {
            if (line.length() > 6 && line[0].isDigit() && !line.startsWith('<')) {
                qStdOut << line << "\n";
                qStdOut.flush();
            } else {
                qStdErr << line << "\n";
            }
        }
        
        qStdErr << "jt9 finished with exit code: " << jt9.exitCode() << "\n";
        qStdErr.flush();
    }
    
    return result;
}
