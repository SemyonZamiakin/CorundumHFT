// Atomic positions under real concurrency: a producer and a consumer thread
// share ONE mapping of the ring, so ThreadSanitizer sees both sides' accesses
// at the same addresses. If publishing (release store of head) or releasing
// (release store of tail) were weaker than release, or the matching loads
// weaker than acquire, TSan reports a data race on the payload bytes:
//   - writer fills a record, reader reads it   -> needs head release/acquire
//   - reader finishes a record, writer reuses it -> needs tail release/acquire
// Without TSan the test still verifies every byte of millions of records,
// which on arm64 (weak memory model, run in CI) catches missing ordering too.
#include "corundum/corundum.h"
#include "check.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

char NAME[64];

std::uint32_t mix(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    return x ^ (x >> 16);
}

// Opens the reader role (so the ring's bookkeeping is right), then points the
// handle at the writer's mapping instead of its own. Restored before closing.
struct SharedView {
    crd_reader r;
    crd_shm *own_shm;
    const unsigned char *own_data;
    explicit SharedView(const crd_writer &w) {
        CHECK_EQ(crd_reader_open(&r, NAME, 0), CRD_OK);
        own_shm = r.shm;
        own_data = r.data;
        r.shm = w.shm;
        r.data = w.data;
    }
    ~SharedView() {
        r.shm = own_shm;
        r.data = own_data;
        crd_reader_close(&r);
    }
};

// Producer: variable sizes, batched publishing; payload = seq, then a pattern.
// Consumer: batched release; every byte checked.
void run(std::uint64_t capacity, std::uint32_t count, std::uint32_t max_size, unsigned batch) {
    CHECK_EQ(crd_create(NAME, capacity, 0600, CRD_CREATE_REPLACE), CRD_OK);
    crd_writer w;
    CHECK_EQ(crd_writer_open(&w, NAME, 0), CRD_OK);
    {
        SharedView view(w);
        std::atomic<unsigned> bad{0};

        std::thread consumer([&] {
            crd_reader &r = view.r;
            std::uint32_t expect = 0, since = 0;
            while (expect < count) {
                crd_msg m;
                crd_status s = crd_read(&r, &m);
                if (s == CRD_EMPTY) {
                    crd_release(&r);
                    since = 0;
                    crd_cpu_relax();
                    continue;
                }
                if (s != CRD_OK) {
                    bad.fetch_add(1000000);
                    return;
                }
                const auto *p = static_cast<const unsigned char *>(m.data);
                std::uint32_t seq;
                std::memcpy(&seq, p, 4);
                if (seq != expect || m.type != (expect & 0xffffU) || m.size != 4 + mix(expect) % (max_size - 3)) {
                    bad.fetch_add(1);
                } else {
                    for (std::uint32_t i = 4; i < m.size; ++i)
                        if (p[i] != static_cast<unsigned char>(seq + i)) {
                            bad.fetch_add(1);
                            break;
                        }
                }
                ++expect;
                if (++since >= batch) {
                    crd_release(&r);
                    since = 0;
                }
            }
            crd_release(&r);
        });

        for (std::uint32_t seq = 0; seq < count; ++seq) {
            const std::uint32_t size = 4 + mix(seq) % (max_size - 3);
            void *dst = nullptr;
            while (crd_reserve(&w, size, &dst) != CRD_OK) {
                crd_publish(&w); // let the consumer see what is staged, then retry
                crd_cpu_relax();
            }
            auto *p = static_cast<unsigned char *>(dst);
            std::memcpy(p, &seq, 4);
            for (std::uint32_t i = 4; i < size; ++i) p[i] = static_cast<unsigned char>(seq + i);
            crd_stage(&w, size, seq & 0xffffU);
            if (seq % batch == batch - 1) crd_publish(&w);
        }
        crd_publish(&w);
        consumer.join();
        CHECK_EQ(bad.load(), 0u);
        CHECK_EQ(crd_reader_pending(&view.r), 0u);
    }
    crd_writer_close(&w);
}

} // namespace

int main(int argc, char **argv) {
    // TSan slows everything ~10x; the sanitizer build passes a smaller count
    const std::uint32_t scale = argc > 1 ? static_cast<std::uint32_t>(std::atoi(argv[1])) : 10;
    crd_test_name(NAME, sizeof NAME, "conc");
    run(4096, 100000 * scale, 200, 1);    // tiny ring: constantly full, positions lap every few records
    run(4096, 50000 * scale, 2040, 4);    // max-size records, wrap on almost every write
    run(1 << 20, 200000 * scale, 64, 16); // large ring, small records, batched
    crd_unlink(NAME);
    return crd_test_report("test_concurrency");
}
