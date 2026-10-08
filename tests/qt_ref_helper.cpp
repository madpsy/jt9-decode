/*
 * Reference side of the shared memory interop test: uses the real Qt
 * QSharedMemory exactly the way jt9 does. Driven by test_shm_interop.
 *
 *   nativekey <key>            print QSharedMemory::nativeKey()
 *   attach-rw <key> <n>        attach, lock, verify pattern A over n bytes,
 *                              write pattern B, print size, unlock, detach
 *   create-wait <key> <size>   create, write pattern A, print READY, wait for
 *                              a line on stdin, then verify pattern B
 *   hold <key> <ms>            attach, lock for <ms>, print LOCKED / RELEASED
 *   contend <key>              attach, print ATTACHED, lock (may block),
 *                              print GOT <ms waited>
 *   lockloop <key> <iters>     attach; repeatedly lock, increment a counter at
 *                              offset 0 non-atomically, unlock
 */

#include <QSharedMemory>
#include <QElapsedTimer>
#include <QThread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

static unsigned char pattern_a(size_t i) { return (unsigned char)((i * 31 + 7) & 0xff); }
static unsigned char pattern_b(size_t i) { return (unsigned char)((i * 17 + 3) & 0xff); }

static int fail(const char *what, QSharedMemory &m) {
    printf("FAIL %s: %s\n", what, m.errorString().toLocal8Bit().constData());
    fflush(stdout);
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 3) return 64;
    std::string cmd = argv[1];
    QSharedMemory m;
    m.setKey(QString::fromUtf8(argv[2]));

    if (cmd == "nativekey") {
        printf("%s\n", m.nativeKey().toLocal8Bit().constData());
        return 0;
    }

    if (cmd == "attach-rw") {
        size_t n = strtoul(argv[3], nullptr, 10);
        if (!m.attach()) return fail("attach", m);
        if (!m.lock()) return fail("lock", m);
        unsigned char *p = (unsigned char *)m.data();
        for (size_t i = 0; i < n; i++) {
            if (p[i] != pattern_a(i)) {
                printf("FAIL mismatch at %zu\n", i);
                m.unlock();
                return 3;
            }
        }
        for (size_t i = 0; i < n; i++) p[i] = pattern_b(i);
        printf("SIZE %d\n", m.size());
        m.unlock();
        m.detach();
        return 0;
    }

    if (cmd == "create-wait") {
        int size = atoi(argv[3]);
        if (!m.create(size)) return fail("create", m);
        m.lock();
        unsigned char *p = (unsigned char *)m.data();
        for (int i = 0; i < size; i++) p[i] = pattern_a(i);
        m.unlock();
        printf("READY\n");
        fflush(stdout);
        std::string line;
        std::getline(std::cin, line);
        m.lock();
        for (int i = 0; i < size; i++) {
            if (p[i] != pattern_b(i)) {
                printf("FAIL mismatch at %d\n", i);
                m.unlock();
                return 3;
            }
        }
        m.unlock();
        printf("OK\n");
        return 0;
    }

    if (cmd == "hold") {
        int ms = atoi(argv[3]);
        if (!m.attach()) return fail("attach", m);
        if (!m.lock()) return fail("lock", m);
        printf("LOCKED\n");
        fflush(stdout);
        QThread::msleep(ms);
        m.unlock();
        printf("RELEASED\n");
        fflush(stdout);
        m.detach();
        return 0;
    }

    if (cmd == "contend") {
        if (!m.attach()) return fail("attach", m);
        printf("ATTACHED\n");
        fflush(stdout);
        QElapsedTimer t;
        t.start();
        if (!m.lock()) return fail("lock", m);
        printf("GOT %lld\n", (long long)t.elapsed());
        fflush(stdout);
        m.unlock();
        m.detach();
        return 0;
    }

    if (cmd == "lockloop") {
        int iters = atoi(argv[3]);
        if (!m.attach()) return fail("attach", m);
        volatile int *counter = (volatile int *)m.data();
        for (int i = 0; i < iters; i++) {
            if (!m.lock()) return fail("lock", m);
            int v = *counter;
            if ((i & 63) == 0) QThread::yieldCurrentThread();
            *counter = v + 1;
            m.unlock();
        }
        printf("DONE\n");
        m.detach();
        return 0;
    }

    return 64;
}
