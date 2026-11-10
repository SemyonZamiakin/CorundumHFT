// C++ wrapper tests: RAII, typed writes, in-place construction, exceptions.
#include "corundum/corundum.hpp"
#include "check.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace {

struct Sample {
    std::uint64_t ts_ns;
    std::uint32_t core;
    float load;
};

struct Big {
    unsigned char bytes[3000];
};

std::string name;

void test_basic() {
    corundum::create(name, 4096, 0600, CRD_CREATE_REPLACE);
    corundum::Writer w(name);
    corundum::Reader r(name);
    CHECK(static_cast<bool>(w));
    CHECK(static_cast<bool>(r));
    CHECK_EQ(w.max_payload(), 2040u);

    CHECK(w.try_write(1, Sample{100, 3, 0.5f}));
    CHECK(w.try_emplace<Sample>(2, Sample{200, 4, 0.75f}));
    corundum::Message m;
    CHECK(r.try_read(m));
    CHECK_EQ(m.type(), 1u);
    const Sample *s = m.as<Sample>();
    CHECK(s != nullptr && s->ts_ns == 100 && s->core == 3);
    CHECK(m.as<std::uint64_t>() == nullptr); // size mismatch
    CHECK(r.try_read(m));
    CHECK_EQ(m.type(), 2u);
    CHECK(m.as<Sample>()->load == 0.75f);
    CHECK(!r.try_read(m));
    r.release();

    CHECK(w.try_emplace<Sample>(4, std::uint64_t{300}, 5u, 1.5f)); // aggregate built from fields
    CHECK(r.try_read(m));
    CHECK(m.as<Sample>()->ts_ns == 300 && m.as<Sample>()->core == 5 && m.as<Sample>()->load == 1.5f);
    r.release();

    CHECK(!w.try_write(3, Big{})); // too big for a 4 KiB ring
    void *p = w.reserve(16);
    CHECK(p != nullptr);
    std::memcpy(p, "0123456789", 10);
    CHECK(w.commit(10, 9));
    CHECK(r.try_read(m));
    CHECK(m.size() == 10 && std::memcmp(m.data(), "0123456789", 10) == 0);
    r.release();
}

void test_move_and_errors() {
    corundum::Writer a(name);
    corundum::Writer b(std::move(a));
    CHECK(!a);
    CHECK(static_cast<bool>(b));
    bool threw = false;
    try {
        corundum::Writer c(name); // b holds the writer role
    } catch (const corundum::error &e) {
        threw = e.status() == CRD_EBUSY;
    }
    CHECK(threw);
    a = std::move(b);
    CHECK(static_cast<bool>(a));

    threw = false;
    try {
        corundum::Reader missing("/crd-test-does-not-exist");
    } catch (const corundum::error &e) {
        threw = e.status() == CRD_ENOENT;
    }
    CHECK(threw);
}

void test_wait_threads() {
    corundum::create(name, 1 << 14, 0600, CRD_CREATE_REPLACE);
    corundum::Reader r(name);
    CHECK(r.wait(std::chrono::milliseconds(5)) == corundum::Reader::wait_result::timeout);
    std::thread producer([] {
        corundum::Writer w(name);
        for (std::uint32_t i = 0; i < 1000; ++i) {
            while (!w.try_write(i, Sample{i, i, 0.f})) std::this_thread::yield();
            w.notify();
            if (i % 100 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }); // Writer destructor closes the stream
    std::uint32_t got = 0, bad = 0;
    for (;;) {
        auto res = r.wait(std::chrono::seconds(2));
        if (res != corundum::Reader::wait_result::ready) {
            CHECK(res == corundum::Reader::wait_result::closed);
            break;
        }
        corundum::Message m;
        while (r.try_read(m)) {
            if (m.type() != got || m.as<Sample>()->ts_ns != got) ++bad;
            ++got;
        }
        r.release();
    }
    producer.join();
    CHECK_EQ(got, 1000u);
    CHECK_EQ(bad, 0u);
    CHECK(r.writer_closed());
}

} // namespace

int main() {
    char buf[64];
    crd_test_name(buf, sizeof buf, "cpp");
    name = buf;
    test_basic();
    test_move_and_errors();
    test_wait_threads();
    corundum::unlink(name);
    return crd_test_report("test_cpp");
}
