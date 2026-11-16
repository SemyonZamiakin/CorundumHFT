// The wire macros in C++, with the C++ wrapper: same layouts as in C.
#include "corundum/corundum.hpp"
#include "corundum/wire.h"
#include "../examples/market_data.h"
#include "check.h"

#include <cstring>
#include <string>

// a C++ layout with a constructor: still trivially copyable and standard-layout
struct Order {
    std::uint64_t id;
    std::int64_t px;
    std::uint32_t qty;
    std::uint16_t venue;
    std::uint8_t side;
    std::uint8_t reserved;
    Order() = default;
    Order(std::uint64_t i, std::int64_t p, std::uint32_t q) : id(i), px(p), qty(q), venue(1), side('B'), reserved(0) {}
};
CRD_WIRE_SIZE(Order, 24);
CRD_WIRE_PACKED(Order, id, px, qty, venue, side, reserved);
CRD_WIRE_OFFSET(Order, venue, 20);

static_assert(sizeof(md_quote) == 48 && offsetof(md_quote, seq) == 36, "same layout as in C");
static_assert(corundum::is_wire_type_v<md_quote>, "readable in place");

int main() {
    char name[64];
    crd_test_name(name, sizeof name, "wirecpp");
    corundum::create(name, 4096, 0600, CRD_CREATE_REPLACE);
    {
        corundum::Writer w(name);
        corundum::Reader r(name);
        int bad = 0;
        for (std::uint32_t i = 0; i < 500; ++i) {
            CHECK(w.try_emplace<Order>(7, i, -5 * static_cast<std::int64_t>(i), i * 2));
            corundum::Message m;
            CHECK(r.try_read(m));
            const Order *o = m.as<Order>();
            const Order *o2 = CRD_MSG_AS(Order, m.native());
            if (!o || o != o2 || o->id != i || o->px != -5 * static_cast<std::int64_t>(i) || o->qty != i * 2 ||
                o->venue != 1 || o->side != 'B')
                ++bad;
            if (m.as<md_quote>() != nullptr) ++bad;
            r.release();
        }
        CHECK_EQ(bad, 0);
    }
    corundum::unlink(name);
    return crd_test_report("test_wire_cpp");
}
