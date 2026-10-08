/*
 * Qt-compatible System V shared memory, without Qt.
 *
 * jt9 is a Qt program: when started with "-s <key>" it calls
 * QSharedMemory::attach(<key>) and then lock()/unlock() around every access.
 * This header reproduces what Qt 5's QSharedMemory (System V backend, the
 * Linux default) does on the wire, so jt9 can attach to memory we create:
 *
 *   native name  = <prefix> + <ASCII letters of key> + hex(sha1(utf8(key)))
 *   key file     = $TMPDIR (or /tmp) + "/" + native name, created 0640
 *   SysV key     = ftok(key file, 'Q')
 *   memory       = shmget(key, size, 0600 | IPC_CREAT | IPC_EXCL)
 *   lock         = one-element SysV semaphore (prefix "qipc_systemsem_"),
 *                  initial value 1, semop -1/+1 with SEM_UNDO
 *
 * Prefixes: "qipc_sharedmemory_" for the memory, "qipc_systemsem_" for the
 * lock. Verified against Qt 5.15 by tests/test_shm_interop.cpp.
 *
 * NOTE: Qt >= 6.6 can be built to use POSIX IPC instead; a jt9 linked
 * against such a Qt will not be able to attach to this memory.
 */

#ifndef QT_SHM_COMPAT_H
#define QT_SHM_COMPAT_H

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <unistd.h>

namespace qtcompat {

// Minimal SHA-1 (FIPS 180-1), only used to derive key names.
inline std::string sha1_hex(const std::string &input) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string msg = input;
    uint64_t bit_len = (uint64_t)input.size() * 8;
    msg += (char)0x80;
    while (msg.size() % 64 != 56) msg += (char)0x00;
    for (int i = 7; i >= 0; i--) msg += (char)((bit_len >> (i * 8)) & 0xff);

    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            const unsigned char *p = (const unsigned char *)msg.data() + chunk + i * 4;
            w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        }
        for (int i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    char out[41];
    for (int i = 0; i < 5; i++) snprintf(out + i * 8, 9, "%08x", h[i]);
    return std::string(out, 40);
}

// Equivalent of QDir::tempPath() on Unix: $TMPDIR (cleaned) or /tmp.
inline std::string temp_path() {
    const char *env = getenv("TMPDIR");
    std::string t = (env && *env) ? env : "/tmp";
    while (t.size() > 1 && t.back() == '/') t.pop_back();
    return t;
}

// Equivalent of QSharedMemoryPrivate::makePlatformSafeKey() (System V build).
inline std::string native_key_path(const std::string &key, const std::string &prefix) {
    if (key.empty()) return std::string();
    std::string result = prefix;
    for (char ch : key) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) result += ch;
    }
    result += sha1_hex(key);
    return temp_path() + "/" + result;
}

// Equivalent of QSharedMemoryPrivate::createUnixKeyFile():
// 1 = created, 0 = already existed, -1 = error.
inline int create_key_file(const std::string &path) {
    int fd = open(path.c_str(), O_EXCL | O_CREAT | O_RDWR, 0640);
    if (fd == -1) return errno == EEXIST ? 0 : -1;
    close(fd);
    return 1;
}

// Equivalent of QSystemSemaphore as used internally by QSharedMemory.
class SystemSemaphore {
public:
    SystemSemaphore() {}
    ~SystemSemaphore() { cleanup(); }
    SystemSemaphore(const SystemSemaphore &) = delete;
    SystemSemaphore &operator=(const SystemSemaphore &) = delete;

    // create == true mirrors QSystemSemaphore::Create: take ownership and
    // force the initial value even if the semaphore was left over.
    bool open(const std::string &key, bool create, std::string &err) {
        cleanup();
        file_ = native_key_path(key, "qipc_systemsem_");
        int built = create_key_file(file_);
        if (built == -1) { err = "cannot create semaphore key file " + file_ + ": " + strerror(errno); return false; }
        created_file_ = (built == 1);
        key_t k = ftok(file_.c_str(), 'Q');
        if (k == -1) { err = std::string("ftok failed for semaphore: ") + strerror(errno); cleanup(); return false; }
        id_ = semget(k, 1, 0600 | IPC_CREAT | IPC_EXCL);
        if (id_ == -1) {
            if (errno == EEXIST) id_ = semget(k, 1, 0600 | IPC_CREAT);
            if (id_ == -1) { err = std::string("semget failed: ") + strerror(errno); cleanup(); return false; }
        } else {
            created_sem_ = true;
            created_file_ = true;
        }
        if (create) { created_sem_ = true; created_file_ = true; }
        if (created_sem_) {
            union { int val; struct semid_ds *buf; unsigned short *array; } arg;
            arg.val = 1;
            if (semctl(id_, 0, SETVAL, arg) == -1) { err = std::string("semctl SETVAL failed: ") + strerror(errno); cleanup(); return false; }
        }
        return true;
    }

    bool acquire() { return modify(-1); }
    bool release() { return modify(1); }

    void cleanup() {
        if (created_file_ && !file_.empty()) unlink(file_.c_str());
        if (created_sem_ && id_ != -1) semctl(id_, 0, IPC_RMID);
        id_ = -1;
        created_file_ = created_sem_ = false;
    }

private:
    bool modify(int count) {
        if (id_ == -1) return false;
        struct sembuf op;
        op.sem_num = 0;
        op.sem_op = (short)count;
        op.sem_flg = SEM_UNDO;
        while (semop(id_, &op, 1) == -1) {
            if (errno != EINTR) return false;
        }
        return true;
    }

    std::string file_;
    int id_ = -1;
    bool created_file_ = false;
    bool created_sem_ = false;
};

// Equivalent of QSharedMemory (System V backend).
class SharedMemory {
public:
    explicit SharedMemory(const std::string &key) : key_(key) {
        native_ = native_key_path(key, "qipc_sharedmemory_");
    }
    ~SharedMemory() { detach(); }
    SharedMemory(const SharedMemory &) = delete;
    SharedMemory &operator=(const SharedMemory &) = delete;

    const std::string &key() const { return key_; }
    const std::string &nativeKey() const { return native_; }
    const std::string &errorString() const { return err_; }
    void *data() const { return data_; }
    size_t size() const { return size_; }
    bool isAttached() const { return data_ != nullptr; }

    bool create(size_t size) {
        if (!sem_.open(key_, true, err_)) return false;
        Locker l(this);
        if (!l.ok) { err_ = "QSharedMemory::create: unable to lock"; return false; }

        int built = create_key_file(native_);
        if (built == -1) { err_ = "cannot create key file " + native_ + ": " + strerror(errno); return false; }
        bool created_file = (built == 1);
        key_t k = ftok(native_.c_str(), 'Q');
        if (k == -1) {
            err_ = std::string("ftok failed: ") + strerror(errno);
            if (created_file) unlink(native_.c_str());
            return false;
        }
        if (shmget(k, size, 0600 | IPC_CREAT | IPC_EXCL) == -1) {
            err_ = errno == EEXIST ? "QSharedMemory::create: already exists"
                                   : std::string("shmget failed: ") + strerror(errno);
            if (created_file) unlink(native_.c_str());
            return false;
        }
        return attach_locked();
    }

    bool attach() {
        if (isAttached()) { err_ = "QSharedMemory::attach: already attached"; return false; }
        if (!sem_.open(key_, false, err_)) return false;
        Locker l(this);
        if (!l.ok) { err_ = "QSharedMemory::attach: unable to lock"; return false; }
        return attach_locked();
    }

    bool detach() {
        if (!isAttached()) return false;
        Locker l(this);
        shmdt(data_);
        data_ = nullptr;
        size_ = 0;
        // Remove the segment and key file once nobody is attached any more.
        key_t k = ftok(native_.c_str(), 'Q');
        if (k != -1) {
            int id = shmget(k, 0, 0400);
            struct shmid_ds ds;
            if (id != -1 && shmctl(id, IPC_STAT, &ds) == 0 && ds.shm_nattch == 0) {
                shmctl(id, IPC_RMID, nullptr);
                unlink(native_.c_str());
            }
        }
        return true;
    }

    bool lock() { return sem_.acquire(); }
    bool unlock() { return sem_.release(); }

private:
    struct Locker {
        explicit Locker(SharedMemory *m) : mem(m), ok(m->lock()) {}
        ~Locker() { if (ok) mem->unlock(); }
        SharedMemory *mem;
        bool ok;
    };

    bool attach_locked() {
        if (access(native_.c_str(), F_OK) != 0) { err_ = "QSharedMemory::attach: UNIX key file doesn't exist"; return false; }
        key_t k = ftok(native_.c_str(), 'Q');
        if (k == -1) { err_ = std::string("ftok failed: ") + strerror(errno); return false; }
        int id = shmget(k, 0, 0600);
        if (id == -1) { err_ = std::string("QSharedMemory::attach: ") + strerror(errno); return false; }
        void *p = shmat(id, nullptr, 0);
        if (p == (void *)-1) { err_ = std::string("shmat failed: ") + strerror(errno); return false; }
        struct shmid_ds ds;
        if (shmctl(id, IPC_STAT, &ds) == -1) {
            err_ = std::string("shmctl failed: ") + strerror(errno);
            shmdt(p);
            return false;
        }
        data_ = p;
        size_ = ds.shm_segsz;
        return true;
    }

    std::string key_;
    std::string native_;
    std::string err_;
    SystemSemaphore sem_;
    void *data_ = nullptr;
    size_t size_ = 0;
};

}  // namespace qtcompat

#endif  // QT_SHM_COMPAT_H
