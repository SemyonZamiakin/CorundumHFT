/* Minimal test harness: no dependencies. */
#ifndef CRD_TEST_CHECK_H
#define CRD_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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

/* Unique shm name per test process so parallel ctest runs do not collide. */
static void crd_test_name(char *buf, size_t n, const char *tag) {
    snprintf(buf, n, "/crd-test-%s-%d", tag, (int)getpid());
}

static int crd_test_report(const char *suite) {
    printf("%s: %d checks, %d failed\n", suite, crd_test_checks, crd_test_failures);
    return crd_test_failures ? 1 : 0;
}

#endif
