/* Minimal test harness and small platform helpers: no dependencies. */
#ifndef CRD_TEST_CHECK_H
#define CRD_TEST_CHECK_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

static int crd_test_failures = 0;
static int crd_test_checks = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++crd_test_checks;                                                            \
        if (!(cond)) {                                                                \
            ++crd_test_failures;                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

static inline int crd_test_pid(void) {
#if defined(_WIN32)
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

/* Unique ring name per test process so parallel ctest runs do not collide. */
static inline void crd_test_name(char *buf, size_t n, const char *tag) {
    snprintf(buf, n, "/crd-test-%s-%d", tag, crd_test_pid());
}

static inline int crd_test_report(const char *suite) {
    printf("%s: %d checks, %d failed\n", suite, crd_test_checks, crd_test_failures);
    return crd_test_failures ? 1 : 0;
}

static inline int64_t crd_test_ms(void) {
#if defined(_WIN32)
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static inline void crd_test_sleep_us(unsigned us) {
#if defined(_WIN32)
    Sleep(us >= 1000 ? us / 1000 : 1); /* Windows cannot sleep below ~1 ms */
#else
    struct timespec ts = {0, (long)us * 1000L};
    nanosleep(&ts, NULL);
#endif
}

/* pid of a process that has already exited */
static inline int crd_test_dead_pid(void) {
#if defined(_WIN32)
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    char cmd[] = "cmd.exe /c exit 0";
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    int pid = (int)pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess); /* last handle: the pid is no longer in use */
    return pid;
#else
    pid_t child = fork();
    if (child == 0) _exit(0);
    waitpid(child, NULL, 0);
    return (int)child;
#endif
}

/* pid of a live process that is not us */
static inline int crd_test_live_foreign_pid(void) {
#if defined(_WIN32)
    return 4; /* "System": always running, not openable by us */
#else
    return (int)getppid();
#endif
}

/* A raw shared segment with the same name a ring would use, for corruption tests. */
typedef struct {
    void *p;
    size_t size;
#if defined(_WIN32)
    HANDLE h;
#else
    int fd;
#endif
} crd_test_seg;

static inline int crd_test_seg_create(crd_test_seg *s, const char *name, size_t size) {
    s->size = size;
#if defined(_WIN32)
    char n[256];
    snprintf(n, sizeof n, "Local\\corundum.%s", name + 1);
    s->h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)size, n);
    if (!s->h) return 0;
    s->p = MapViewOfFile(s->h, FILE_MAP_ALL_ACCESS, 0, 0, size);
    return s->p != NULL;
#else
    s->fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (s->fd < 0) return 0;
    if (ftruncate(s->fd, (off_t)size) != 0) return 0;
    s->p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
    return s->p != MAP_FAILED;
#endif
}

static inline void crd_test_seg_destroy(crd_test_seg *s, const char *name) {
#if defined(_WIN32)
    (void)name;
    UnmapViewOfFile(s->p);
    CloseHandle(s->h);
#else
    munmap(s->p, s->size);
    close(s->fd);
    shm_unlink(name);
#endif
}

#endif
