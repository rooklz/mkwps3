// lv2 system calls behind the compat <mutex>, <thread>, <condition_variable>
// and <filesystem>. This is the only translation unit that includes the
// PSL1GHT headers for them (their s32/u32 typedefs clash with aurora's Wii
// types), so everything else talks to the kernel through these.

#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ppu-types.h>
#include <sys/mutex.h>
#include <sys/cond.h>
#include <sys/thread.h>
#include <sys/systime.h>
#include <lv2/sysfs.h>
#include <sys/file.h>
#include <sys/process.h>
#include <net/netdb.h>
#ifndef EAI_NONAME
#define EAI_NONAME -2
#endif
#ifndef EAI_MEMORY
#define EAI_MEMORY -10
#endif
#include <net/socket.h>
#include <netinet/in.h>
#include <cstdlib>
#include <sys/memory.h>
#include <sys/dbg.h>
#include <ppu-lv2.h>

// The primary thread only supervises (main_ps3.cpp); the guest gets its own
// thread with a large stack.
SYS_PROCESS_PARAM(1001, 0x100000)

extern "C" {

int mkw_lv2_mutex_create(uint32_t* id, int recursive)
{
    sys_mutex_attr_t attr;
    sysMutexAttrInitialize(attr);
    attr.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO;
    attr.attr_recursive = recursive ? SYS_MUTEX_ATTR_RECURSIVE : SYS_MUTEX_ATTR_NOT_RECURSIVE;
    sys_mutex_t m = 0;
    const int rc = sysMutexCreate(&m, &attr);
    *id = m;
    return rc;
}

int mkw_lv2_mutex_destroy(uint32_t id) { return sysMutexDestroy(id); }
int mkw_lv2_mutex_lock(uint32_t id) { return sysMutexLock(id, 0); }
int mkw_lv2_mutex_trylock(uint32_t id) { return sysMutexTryLock(id); }
int mkw_lv2_mutex_unlock(uint32_t id) { return sysMutexUnlock(id); }

int mkw_lv2_cond_create(uint32_t* id, uint32_t mutex)
{
    sys_cond_attr_t attr;
    sysCondAttrInitialize(attr);
    sys_cond_t c = 0;
    const int rc = sysCondCreate(&c, mutex, &attr);
    *id = c;
    return rc;
}

int mkw_lv2_cond_destroy(uint32_t id) { return sysCondDestroy(id); }
int mkw_lv2_cond_wait(uint32_t id, uint64_t timeout_usec) { return sysCondWait(id, timeout_usec); }
int mkw_lv2_cond_signal(uint32_t id) { return sysCondSignal(id); }
int mkw_lv2_cond_broadcast(uint32_t id) { return sysCondBroadcast(id); }

// newlib keeps stdin/stdout/stderr per thread here, so a log redirected on the
// primary thread does not reach the threads it starts: their printf goes to
// the debug TTY. (C++ streams are fine -- libstdc++ binds std::cout/cerr to
// the primary thread's FILEs once.) The C stdio entry points are wrapped at
// link time (Makefile.ps3) so a write to any thread's stdout/stderr lands in
// the primary thread's redirected FILE, the way libstdc++ does it.
static FILE* g_logOut = nullptr;
static FILE* g_logErr = nullptr;

void mkw_log_streams_set(FILE* out, FILE* err)
{
    g_logOut = out;
    g_logErr = err;
}

static FILE* RouteStream(FILE* fp)
{
    if (g_logOut && fp == stdout)
        return g_logOut;
    if (g_logErr && fp == stderr)
        return g_logErr;
    return fp;
}

int __real_vfprintf(FILE* fp, const char* fmt, va_list ap);
int __real_fputs(const char* s, FILE* fp);
int __real_fputc(int c, FILE* fp);
size_t __real_fwrite(const void* p, size_t size, size_t n, FILE* fp);
int __real_fflush(FILE* fp);

int __wrap_vfprintf(FILE* fp, const char* fmt, va_list ap) { return __real_vfprintf(RouteStream(fp), fmt, ap); }
int __wrap_vprintf(const char* fmt, va_list ap) { return __real_vfprintf(RouteStream(stdout), fmt, ap); }

int __wrap_fprintf(FILE* fp, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = __real_vfprintf(RouteStream(fp), fmt, ap);
    va_end(ap);
    return n;
}

int __wrap_printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = __real_vfprintf(RouteStream(stdout), fmt, ap);
    va_end(ap);
    return n;
}

int __wrap_fputs(const char* s, FILE* fp) { return __real_fputs(s, RouteStream(fp)); }
int __wrap_fputc(int c, FILE* fp) { return __real_fputc(c, RouteStream(fp)); }
int __wrap_putchar(int c) { return __real_fputc(c, RouteStream(stdout)); }
size_t __wrap_fwrite(const void* p, size_t size, size_t n, FILE* fp) { return __real_fwrite(p, size, n, RouteStream(fp)); }
int __wrap_fflush(FILE* fp) { return __real_fflush(fp ? RouteStream(fp) : fp); }

int __wrap_puts(const char* s)
{
    FILE* out = RouteStream(stdout);
    if (__real_fputs(s, out) < 0)
        return -1;
    return __real_fputc('\n', out);
}

int mkw_lv2_thread_create(uint64_t* id, void (*entry)(void*), void* arg, int priority,
                          uint64_t stack, const char* name)
{
    sys_ppu_thread_t t = 0;
    char nameBuf[28];
    std::strncpy(nameBuf, name ? name : "mkw", sizeof nameBuf - 1);
    nameBuf[sizeof nameBuf - 1] = '\0';
    const int rc = sysThreadCreate(&t, entry, arg, priority, stack, THREAD_JOINABLE, nameBuf);
    *id = t;
    return rc;
}

// Free and total user memory (bytes), for the stats report:
// sys_memory_get_user_memory_size (syscall 352).
static s64 MemoryInfoSyscall(void* info)
{
    lv2syscall1(352, reinterpret_cast<u64>(info));
    return_to_user_prog(s64);
}

void mkw_lv2_memory(uint64_t* freeBytes, uint64_t* totalBytes)
{
    struct {
        u32 total;
        u32 available;
    } info = {0, 0};
    if (MemoryInfoSyscall(&info) == 0) {
        *freeBytes = info.available;
        *totalBytes = info.total;
    } else {
        *freeBytes = *totalBytes = 0;
    }
}

// The calling thread's lv2 name, for fatal reports.
void mkw_lv2_thread_name(char* out, unsigned cap)
{
    sys_ppu_thread_t self = 0;
    sysThreadGetId(&self);
    char name[32] = {0};
    sysDbgGetPPUThreadName(self, name);
    std::strncpy(out, name, cap - 1);
    out[cap - 1] = '\0';
}

int mkw_lv2_thread_join(uint64_t id)
{
    u64 rv = 0;
    return sysThreadJoin(id, &rv);
}

void mkw_lv2_thread_exit(void) { sysThreadExit(0); }
void mkw_lv2_thread_yield(void) { sysThreadYield(); }

uint64_t mkw_lv2_thread_self(void)
{
    sys_ppu_thread_t id = 0;
    sysThreadGetId(&id);
    return id;
}

void mkw_lv2_usleep(uint32_t us) { sysUsleep(us); }

int mkw_fs_stat(const char* path, int* mode, uint64_t* size, int64_t* mtime)
{
    sysFSStat st;
    const int rc = sysLv2FsStat(path, &st);
    if (rc != 0)
        return rc;
    *mode = st.st_mode;
    *size = st.st_size;
    *mtime = static_cast<int64_t>(st.st_mtime);
    return 0;
}

// Reads [offset, offset + length) of a file straight into `dst` -- the DVD
// HLE's disc reads. A few descriptors stay open: the game streams large files
// in chunks, and an open per chunk costs more than the read. Returns 0, -2 for
// a missing file, or -1 for a short read or I/O error.
namespace {
struct OpenFile {
    char path[256];
    s32 fd;
    u64 lastUse;
};
OpenFile g_openFiles[8];
u64 g_openClock = 0;
u32 g_openLock = 0;
}

int mkw_fs_read_range(const char* path, uint64_t offset, void* dst, uint32_t length)
{
    if (!g_openLock)
        mkw_lv2_mutex_create(&g_openLock, 0);
    mkw_lv2_mutex_lock(g_openLock);
    OpenFile* slot = nullptr;
    OpenFile* victim = &g_openFiles[0];
    for (OpenFile& f : g_openFiles) {
        if (f.fd > 0 && std::strcmp(f.path, path) == 0) {
            slot = &f;
            break;
        }
        if (f.fd <= 0 || f.lastUse < victim->lastUse)
            victim = &f;
    }
    if (!slot) {
        if (victim->fd > 0)
            sysLv2FsClose(victim->fd);
        victim->fd = 0;
        s32 fd = -1;
        if (sysLv2FsOpen(path, SYS_O_RDONLY, &fd, 0, nullptr, 0) != 0 || fd <= 0) {
            mkw_lv2_mutex_unlock(g_openLock);
            return -2;
        }
        std::strncpy(victim->path, path, sizeof victim->path - 1);
        victim->path[sizeof victim->path - 1] = '\0';
        victim->fd = fd;
        slot = victim;
    }
    slot->lastUse = ++g_openClock;
    u64 pos = 0;
    int rc = 0;
    if (sysLv2FsLSeek64(slot->fd, offset, 0, &pos) != 0 || pos != offset) {
        rc = -1;
    } else {
        u8* out = static_cast<u8*>(dst);
        u32 left = length;
        while (left) {
            u64 got = 0;
            if (sysLv2FsRead(slot->fd, out, left, &got) != 0 || got == 0) {
                rc = -1;
                break;
            }
            out += got;
            left -= static_cast<u32>(got);
        }
    }
    mkw_lv2_mutex_unlock(g_openLock);
    return rc;
}

// newlib's rename() is not implemented on lv2; every caller (the NAND HLE's
// write-then-rename saves above all) is wrapped onto the syscall. POSIX
// replaces an existing target; lv2 refuses, so it goes first.
int __wrap_rename(const char* from, const char* to)
{
    sysFSStat st;
    if (sysLv2FsStat(to, &st) == 0 && (st.st_mode & 0170000) != 0040000)
        sysLv2FsUnlink(to);
    return sysLv2FsRename(from, to) == 0 ? 0 : -1;
}

// newlib's fsync() is not implemented either; the NAND HLE fsyncs before each
// safe-save commit and skips the commit when it fails. A newlib descriptor on
// lv2 is the lv2 descriptor.
int __wrap_fsync(int fd)
{
    return sysLv2FsFsync(fd) == 0 ? 0 : -1;
}

void freeaddrinfo(struct addrinfo* res);

// getaddrinfo() over PSL1GHT's gethostbyname(): IPv4, one result per address,
// numeric service ports only -- what the Wii network HLE asks for.
int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res)
{
    *res = nullptr;
    const int sockType = hints ? hints->ai_socktype : 0;
    const int protocol = hints ? hints->ai_protocol : 0;
    const uint16_t port = service ? static_cast<uint16_t>(std::atoi(service)) : 0;
    struct hostent* host = node ? gethostbyname(node) : nullptr;
    if (node && (!host || host->h_addrtype != AF_INET || !host->h_addr_list))
        return EAI_NONAME;
    struct addrinfo* head = nullptr;
    struct addrinfo** tail = &head;
    for (int i = 0; !node || host->h_addr_list[i]; ++i) {
        auto* ai = static_cast<struct addrinfo*>(std::calloc(1, sizeof(struct addrinfo) + sizeof(struct sockaddr_in)));
        if (!ai) {
            freeaddrinfo(head);
            return EAI_MEMORY;
        }
        auto* sin = reinterpret_cast<struct sockaddr_in*>(ai + 1);
        sin->sin_family = AF_INET;
        sin->sin_port = htons(port);
        if (node)
            std::memcpy(&sin->sin_addr, host->h_addr_list[i], sizeof sin->sin_addr);
        ai->ai_family = AF_INET;
        ai->ai_socktype = sockType;
        ai->ai_protocol = protocol;
        ai->ai_addrlen = sizeof(struct sockaddr_in);
        ai->ai_addr = reinterpret_cast<struct sockaddr*>(sin);
        *tail = ai;
        tail = &ai->ai_next;
        if (!node)
            break;
    }
    *res = head;
    return head ? 0 : EAI_NONAME;
}

void freeaddrinfo(struct addrinfo* res)
{
    while (res) {
        struct addrinfo* next = res->ai_next;
        std::free(res);
        res = next;
    }
}

const char* gai_strerror(int code)
{
    return code == EAI_MEMORY ? "out of memory" : "name not resolved";
}

int mkw_fs_mkdir(const char* path) { return sysLv2FsMkdir(path, 0755); }
int mkw_fs_rmdir(const char* path) { return sysLv2FsRmdir(path); }
int mkw_fs_unlink(const char* path) { return sysLv2FsUnlink(path); }
int mkw_fs_rename(const char* from, const char* to) { return sysLv2FsRename(from, to); }

int mkw_fs_opendir(const char* path, int* fd)
{
    s32 d = -1;
    const int rc = sysLv2FsOpenDir(path, &d);
    *fd = d;
    return rc;
}

int mkw_fs_readdir(int fd, char* name, unsigned cap)
{
    sysFSDirent ent;
    u64 read = 0;
    if (sysLv2FsReadDir(fd, &ent, &read) != 0)
        return -1;
    if (read == 0)
        return 0;
    const unsigned n = ent.d_namlen < cap - 1 ? ent.d_namlen : cap - 1;
    std::memcpy(name, ent.d_name, n);
    name[n] = '\0';
    return 1;
}

int mkw_fs_closedir(int fd) { return sysLv2FsCloseDir(fd); }

} // extern "C"
