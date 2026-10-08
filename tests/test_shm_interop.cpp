/*
 * Interop tests: qtcompat::SharedMemory (no Qt) against real QSharedMemory
 * (tests/qt_ref_helper, linked with the same Qt5Core jt9 uses).
 *
 * Usage: test_shm_interop <path-to-qt_ref_helper>
 */

#include "qt_shm_compat.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using qtcompat::SharedMemory;

static std::string helper;
static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; fprintf(stderr, "  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
                   fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } \
} while (0)

static unsigned char pattern_a(size_t i) { return (unsigned char)((i * 31 + 7) & 0xff); }
static unsigned char pattern_b(size_t i) { return (unsigned char)((i * 17 + 3) & 0xff); }

static long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Child process with pipes to its stdin/stdout
struct Helper {
    pid_t pid = -1;
    FILE *out = nullptr;
    int in_fd = -1;

    explicit Helper(const std::vector<std::string> &args) {
        int to_child[2], from_child[2];
        if (pipe(to_child) || pipe(from_child)) abort();
        pid = fork();
        if (pid == 0) {
            dup2(to_child[0], 0);
            dup2(from_child[1], 1);
            close(to_child[1]);
            close(from_child[0]);
            std::vector<char *> argv;
            argv.push_back(const_cast<char *>(helper.c_str()));
            for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
            argv.push_back(nullptr);
            execv(helper.c_str(), argv.data());
            _exit(127);
        }
        close(to_child[0]);
        close(from_child[1]);
        in_fd = to_child[1];
        out = fdopen(from_child[0], "r");
    }

    std::string line() {
        char buf[1024];
        if (!fgets(buf, sizeof(buf), out)) return "";
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        return s;
    }

    void send(const std::string &s) {
        ssize_t r = write(in_fd, s.data(), s.size());
        (void)r;
    }

    int wait() {
        if (in_fd != -1) { close(in_fd); in_fd = -1; }
        int status;
        waitpid(pid, &status, 0);
        if (out) { fclose(out); out = nullptr; }
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
};

static std::string run_capture(const std::vector<std::string> &args, int *rc = nullptr) {
    Helper h(args);
    std::string all, l;
    while (!(l = h.line()).empty()) all += l + "\n";
    int r = h.wait();
    if (rc) *rc = r;
    return all;
}

static std::string unique_key(const char *tag) {
    static int n = 0;
    return std::string("JT9DECODE_") + std::to_string(getpid()) + "_" + tag + "_" + std::to_string(n++);
}

static bool ipc_gone(key_t k) {
    errno = 0;
    bool shm = shmget(k, 0, 0) == -1 && errno == ENOENT;
    errno = 0;
    return shm;
}

static bool sem_gone(key_t k) {
    errno = 0;
    return semget(k, 1, 0) == -1 && errno == ENOENT;
}

// ---------------------------------------------------------------------------

static void test_sha1() {
    fprintf(stderr, "sha1 known vectors\n");
    CHECK(qtcompat::sha1_hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "empty");
    CHECK(qtcompat::sha1_hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d", "abc");
    CHECK(qtcompat::sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "448-bit");
    std::string million(1000000, 'a');
    CHECK(qtcompat::sha1_hex(million) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f", "1M a");
}

static void test_native_key_matches_qt() {
    fprintf(stderr, "native key path matches QSharedMemory::nativeKey()\n");
    std::vector<std::string> keys = {
        "JT9DECODE_12345_1791457185463",
        "JT9DECODE_1_2",
        "a", "Z9", "___", "0123456789",
        "mixed-Case.Key with spaces!",
        std::string(55, 'x'), std::string(56, 'y'), std::string(63, 'q'), std::string(64, 'r'),
        std::string(200, 'k') + "_tail",
    };
    // Long keys cover SHA-1 padding boundaries (55/56/63/64 bytes)
    for (const char *tmpdir : {(const char *)nullptr, "/tmp", "/tmp/", "/var/tmp"}) {
        if (tmpdir) setenv("TMPDIR", tmpdir, 1); else unsetenv("TMPDIR");
        for (auto &k : keys) {
            std::string qt = run_capture({"nativekey", k});
            while (!qt.empty() && qt.back() == '\n') qt.pop_back();
            std::string ours = qtcompat::native_key_path(k, "qipc_sharedmemory_");
            CHECK(qt == ours, "TMPDIR=%s key='%s' qt='%s' ours='%s'", tmpdir ? tmpdir : "(unset)",
                  k.c_str(), qt.c_str(), ours.c_str());
        }
    }
    unsetenv("TMPDIR");
}

static void test_we_create_qt_attaches() {
    fprintf(stderr, "we create, Qt attaches/locks/reads/writes\n");
    // Include the real dec_data_t size used by jt9_decode
    for (size_t size : {(size_t)4096, (size_t)1, (size_t)48275376}) {
        std::string key = unique_key("create");
        SharedMemory m(key);
        CHECK(m.create(size), "create(%zu): %s", size, m.errorString().c_str());
        if (!m.isAttached()) continue;
        CHECK(m.size() == size, "size %zu != %zu", m.size(), size);
        size_t n = std::min<size_t>(size, 1 << 20);
        m.lock();
        unsigned char *p = (unsigned char *)m.data();
        for (size_t i = 0; i < n; i++) p[i] = pattern_a(i);
        m.unlock();

        int rc;
        std::string out = run_capture({"attach-rw", key, std::to_string(n)}, &rc);
        CHECK(rc == 0, "helper rc=%d out=%s", rc, out.c_str());
        CHECK(out == "SIZE " + std::to_string(size) + "\n", "helper out=%s", out.c_str());

        m.lock();
        bool ok = true;
        for (size_t i = 0; i < n; i++) if (p[i] != pattern_b(i)) { ok = false; break; }
        m.unlock();
        CHECK(ok, "Qt's writes not visible to us (size %zu)", size);
    }
}

static void test_qt_creates_we_attach() {
    fprintf(stderr, "Qt creates, we attach/lock/read/write\n");
    std::string key = unique_key("qtcreate");
    const int size = 65536;
    Helper h({"create-wait", key, std::to_string(size)});
    std::string ready = h.line();
    CHECK(ready == "READY", "helper said '%s'", ready.c_str());
    {
        SharedMemory m(key);
        CHECK(m.attach(), "attach: %s", m.errorString().c_str());
        if (m.isAttached()) {
            CHECK(m.size() == (size_t)size, "size %zu", m.size());
            CHECK(m.lock(), "lock");
            unsigned char *p = (unsigned char *)m.data();
            bool ok = true;
            for (int i = 0; i < size; i++) if (p[i] != pattern_a(i)) { ok = false; break; }
            CHECK(ok, "Qt's pattern not visible to us");
            for (int i = 0; i < size; i++) p[i] = pattern_b(i);
            CHECK(m.unlock(), "unlock");
        }
    }  // we detach; Qt still attached so the segment must survive
    h.send("go\n");
    std::string ok = h.line();
    int rc = h.wait();
    CHECK(rc == 0 && ok == "OK", "helper rc=%d said '%s'", rc, ok.c_str());

    // Qt was the creator and last to detach: everything must be gone
    std::string shm_file = qtcompat::native_key_path(key, "qipc_sharedmemory_");
    std::string sem_file = qtcompat::native_key_path(key, "qipc_systemsem_");
    CHECK(access(shm_file.c_str(), F_OK) != 0, "%s left behind", shm_file.c_str());
    CHECK(access(sem_file.c_str(), F_OK) != 0, "%s left behind", sem_file.c_str());
}

static void test_lock_semantics() {
    fprintf(stderr, "lock is shared with Qt in both directions\n");
    std::string key = unique_key("lock");
    SharedMemory m(key);
    CHECK(m.create(64), "create: %s", m.errorString().c_str());

    // 1) We hold the lock: Qt's attach() (which locks internally) must wait.
    CHECK(m.lock(), "lock");
    long long t0 = now_ms();
    Helper h({"contend", key});
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    m.unlock();
    std::string attached = h.line();
    long long waited = now_ms() - t0;
    std::string got = h.line();
    int rc = h.wait();
    CHECK(attached == "ATTACHED" && got.rfind("GOT ", 0) == 0 && rc == 0,
          "helper output '%s' '%s' rc=%d", attached.c_str(), got.c_str(), rc);
    CHECK(waited >= 550, "Qt did not wait for our lock (waited %lld ms)", waited);

    // 2) Qt holds the lock: our lock() must wait for it.
    Helper hold({"hold", key, "700"});
    CHECK(hold.line() == "LOCKED", "hold did not lock");
    t0 = now_ms();
    CHECK(m.lock(), "lock");
    waited = now_ms() - t0;
    m.unlock();
    CHECK(hold.line() == "RELEASED", "hold did not release");
    CHECK(hold.wait() == 0, "hold rc");
    CHECK(waited >= 500, "we did not wait for Qt's lock (waited %lld ms)", waited);
}

static void test_mutual_exclusion_hammer() {
    fprintf(stderr, "mutual exclusion under contention (us + 3 Qt processes)\n");
    std::string key = unique_key("hammer");
    SharedMemory m(key);
    CHECK(m.create(64), "create: %s", m.errorString().c_str());
    volatile int *counter = (volatile int *)m.data();
    *counter = 0;

    const int iters = 20000;
    std::vector<Helper *> hs;
    for (int i = 0; i < 3; i++) hs.push_back(new Helper({"lockloop", key, std::to_string(iters)}));
    for (int i = 0; i < iters; i++) {
        m.lock();
        int v = *counter;
        if ((i & 63) == 0) std::this_thread::yield();
        *counter = v + 1;
        m.unlock();
    }
    for (auto *h : hs) {
        std::string done = h->line();
        CHECK(done == "DONE" && h->wait() == 0, "lockloop helper failed: %s", done.c_str());
        delete h;
    }
    CHECK(*counter == iters * 4, "lost updates: counter=%d expected=%d", *counter, iters * 4);
}

static void test_cleanup() {
    fprintf(stderr, "our create/destroy leaves no files, segments or semaphores\n");
    std::string key = unique_key("cleanup");
    std::string shm_file = qtcompat::native_key_path(key, "qipc_sharedmemory_");
    std::string sem_file = qtcompat::native_key_path(key, "qipc_systemsem_");
    key_t shm_key, sem_key;
    {
        SharedMemory m(key);
        CHECK(m.create(4096), "create: %s", m.errorString().c_str());
        shm_key = ftok(shm_file.c_str(), 'Q');
        sem_key = ftok(sem_file.c_str(), 'Q');
        CHECK(shm_key != -1 && sem_key != -1, "ftok");
        struct stat st;
        CHECK(stat(shm_file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0640, "key file mode");
        // Qt attaches and detaches while we are still attached: must not destroy it
        int rc;
        std::string out = run_capture({"hold", key, "1"}, &rc);
        CHECK(rc == 0, "hold rc=%d", rc);
        CHECK(!ipc_gone(shm_key), "segment destroyed while we were attached");
    }
    CHECK(access(shm_file.c_str(), F_OK) != 0, "%s left behind", shm_file.c_str());
    CHECK(access(sem_file.c_str(), F_OK) != 0, "%s left behind", sem_file.c_str());
    CHECK(ipc_gone(shm_key), "shm segment left behind");
    CHECK(sem_gone(sem_key), "semaphore left behind");
}

static void test_stale_leftovers() {
    fprintf(stderr, "stale key files / semaphore from a crashed run are handled\n");
    std::string key = unique_key("stale");
    std::string shm_file = qtcompat::native_key_path(key, "qipc_sharedmemory_");
    std::string sem_file = qtcompat::native_key_path(key, "qipc_systemsem_");
    // Simulate a crash: files exist and the semaphore exists with value 0 (held)
    qtcompat::create_key_file(shm_file);
    qtcompat::create_key_file(sem_file);
    key_t sk = ftok(sem_file.c_str(), 'Q');
    int sid = semget(sk, 1, 0600 | IPC_CREAT);
    union { int val; } arg; arg.val = 0;
    semctl(sid, 0, SETVAL, arg);

    pid_t pid = fork();
    if (pid == 0) {
        alarm(5);  // a hang here means the stale held semaphore wasn't reset
        bool ok;
        {
            SharedMemory m(key);
            ok = m.create(128);
        }
        _exit(ok ? 0 : 1);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "create over stale leftovers failed/hung (status %d)", status);
    CHECK(access(shm_file.c_str(), F_OK) != 0, "%s left behind", shm_file.c_str());
    CHECK(access(sem_file.c_str(), F_OK) != 0, "%s left behind", sem_file.c_str());
    CHECK(sem_gone(sk), "stale semaphore left behind");
}

static void test_attach_missing() {
    fprintf(stderr, "attach to a key that doesn't exist fails cleanly\n");
    std::string key = unique_key("missing");
    {
        SharedMemory m(key);
        CHECK(!m.attach(), "attach should fail");
        CHECK(!m.isAttached(), "should not be attached");
    }
    CHECK(access(qtcompat::native_key_path(key, "qipc_systemsem_").c_str(), F_OK) != 0, "sem file left behind");
    CHECK(access(qtcompat::native_key_path(key, "qipc_sharedmemory_").c_str(), F_OK) != 0, "shm file left behind");
}

static void test_create_twice() {
    fprintf(stderr, "second create of the same key fails, first stays intact\n");
    std::string key = unique_key("twice");
    SharedMemory a(key);
    CHECK(a.create(128), "create a");
    ((char *)a.data())[0] = 42;
    {
        SharedMemory b(key);
        CHECK(!b.create(128), "second create should fail");
    }
    int rc;
    run_capture({"hold", key, "1"}, &rc);
    CHECK(rc == 0, "Qt can no longer attach after failed second create (rc=%d)", rc);
    CHECK(((char *)a.data())[0] == 42, "data damaged");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <qt_ref_helper>\n", argv[0]);
        return 2;
    }
    helper = argv[1];
    signal(SIGPIPE, SIG_IGN);

    test_sha1();
    test_native_key_matches_qt();
    test_we_create_qt_attaches();
    test_qt_creates_we_attach();
    test_lock_semantics();
    test_mutual_exclusion_hammer();
    test_cleanup();
    test_stale_leftovers();
    test_attach_missing();
    test_create_twice();

    fprintf(stderr, "\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
