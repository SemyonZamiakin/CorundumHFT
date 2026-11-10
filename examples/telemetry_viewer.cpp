// GUI-side consumer (console stand-in): sleeps on a futex while idle, reads
// samples in place, and redraws ten times per second with the latest values.
//
//   telemetry_viewer
#include "corundum/corundum.hpp"
#include "telemetry.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <time.h>

int main() {
    using namespace std::chrono;
    corundum::Reader reader;
    for (int attempt = 0;; ++attempt) {
        try {
            reader = corundum::Reader(TELEMETRY_RING);
            break;
        } catch (const corundum::error &e) {
            if (attempt == 50) {
                std::fprintf(stderr, "telemetry_viewer: %s (is telemetry_daemon running?)\n", e.what());
                return 1;
            }
            std::this_thread::sleep_for(milliseconds(100));
        }
    }

    telemetry_sample last{};
    std::uint64_t received = 0, lost = 0, expected = 0;
    auto next_draw = steady_clock::now();
    for (;;) {
        auto res = reader.wait(milliseconds(100));
        if (res == corundum::Reader::wait_result::closed) break;
        corundum::Message m;
        while (reader.try_read(m)) {
            if (const auto *s = m.as<telemetry_sample>()) {
                if (received && s->seq != expected) lost += s->seq - expected;
                expected = s->seq + 1;
                last = *s; // keep a copy: the slot is reused after release()
                ++received;
            }
        }
        reader.release();

        if (steady_clock::now() >= next_draw && received) {
            next_draw += milliseconds(100);
            timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            const double age_us =
                (double)((std::uint64_t)now.tv_sec * 1000000000ull + (std::uint64_t)now.tv_nsec - last.mono_ns) / 1e3;
            std::printf("\rsamples %-9llu lost %-5llu mem %5.1f%%  cpu:", (unsigned long long)received,
                        (unsigned long long)lost, last.mem_used_permille / 10.0);
            for (std::uint32_t i = 0; i < last.cpu_count && i < 16; ++i)
                std::printf(" %3u", last.cpu_busy_permille[i] / 10);
            std::printf("  age %.0f us   ", age_us);
            std::fflush(stdout);
        }
    }
    std::printf("\ndaemon closed the stream after %llu samples\n", (unsigned long long)received);
    return 0;
}
