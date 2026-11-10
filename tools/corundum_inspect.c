/*
 * corundum-inspect: prints the state of a ring without attaching to it.
 *
 *   corundum-inspect /ring-name
 */
#include "corundum/corundum.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s /ring-name\n", argv[0]);
        return 2;
    }
    crd_info info;
    crd_status s = crd_inspect(argv[1], &info);
    if (s != CRD_OK) {
        fprintf(stderr, "%s: %s%s%s\n", argv[1], crd_strerror(s), s == CRD_ESYS ? ": " : "",
                s == CRD_ESYS ? strerror(errno) : "");
        return 1;
    }
    printf("ring        %s\n", argv[1]);
    printf("capacity    %llu bytes\n", (unsigned long long)info.capacity);
    printf("used        %llu bytes (%.1f%%)\n", (unsigned long long)info.used_bytes,
           100.0 * (double)info.used_bytes / (double)info.capacity);
    printf("head        %llu\n", (unsigned long long)info.head);
    printf("tail        %llu\n", (unsigned long long)info.tail);
    printf("writer      %s", info.writer_pid ? "pid " : "none");
    if (info.writer_pid) printf("%d", info.writer_pid);
    printf("%s\n", info.writer_closed ? " (stream closed)" : "");
    printf("reader      %s", info.reader_pid ? "pid " : "none");
    if (info.reader_pid) printf("%d", info.reader_pid);
    printf("%s\n", info.reader_waiting ? " (sleeping in crd_wait)" : "");
    return 0;
}
