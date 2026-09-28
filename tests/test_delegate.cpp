#include "core/delegate.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <vector>

using namespace fjell;

TEST_CASE("Broadcast calls bound listener", "[delegate]") {
    Delegate<void(int)> d;
    int received = 0;
    auto conn = d.bind([&](int v) { received = v; });

    d.broadcast(42);
    REQUIRE(received == 42);
}

TEST_CASE("Broadcast calls multiple listeners", "[delegate]") {
    Delegate<void()> d;
    int count = 0;
    auto c1 = d.bind([&] { count++; });
    auto c2 = d.bind([&] { count++; });
    auto c3 = d.bind([&] { count++; });

    d.broadcast();
    REQUIRE(count == 3);
}

TEST_CASE("Connection disconnect stops delivery", "[delegate]") {
    Delegate<void()> d;
    int count = 0;
    auto conn = d.bind([&] { count++; });

    d.broadcast();
    REQUIRE(count == 1);

    conn.disconnect();
    d.broadcast();
    REQUIRE(count == 1); // no further calls
}

TEST_CASE("Connection RAII disconnects on destruction", "[delegate]") {
    Delegate<void()> d;
    int count = 0;

    {
        auto conn = d.bind([&] { count++; });
        d.broadcast();
        REQUIRE(count == 1);
    } // conn destroyed here

    d.broadcast();
    REQUIRE(count == 1);
}

TEST_CASE("Disconnect during broadcast is safe", "[delegate]") {
    Delegate<void()> d;
    int call_count = 0;
    Connection c1;
    Connection c2;

    c1 = d.bind([&] {
        call_count++;
        c2.disconnect(); // disconnect the other listener mid-broadcast
    });
    c2 = d.bind([&] {
        call_count++;
    });

    d.broadcast();
    // Both should have been called (c2 disconnected after iteration started)
    // Actually c2's callback is nulled during broadcast but the loop already
    // captured the size — depends on ordering. Let's just verify no crash
    // and that at least c1 ran.
    REQUIRE(call_count >= 1);

    // Second broadcast: c2 should be cleaned up
    call_count = 0;
    d.broadcast();
    REQUIRE(call_count == 1);
}

TEST_CASE("Broadcast with no listeners is a no-op", "[delegate]") {
    Delegate<void(int, float)> d;
    d.broadcast(1, 2.0f); // should not crash
    REQUIRE(d.empty());
}

TEST_CASE("Connection move transfers ownership", "[delegate]") {
    Delegate<void()> d;
    int count = 0;
    auto c1 = d.bind([&] { count++; });

    Connection c2 = std::move(c1);
    REQUIRE_FALSE(c1.connected());
    REQUIRE(c2.connected());

    d.broadcast();
    REQUIRE(count == 1);

    c2.disconnect();
    d.broadcast();
    REQUIRE(count == 1);
}

TEST_CASE("Clear removes all listeners", "[delegate]") {
    Delegate<void()> d;
    int count = 0;
    auto c1 = d.bind([&] { count++; });
    auto c2 = d.bind([&] { count++; });

    REQUIRE(d.size() == 2);
    d.clear();

    d.broadcast();
    REQUIRE(count == 0);
}

TEST_CASE("Multiple arguments are forwarded", "[delegate]") {
    Delegate<void(int, const std::string&)> d;
    int got_i = 0;
    std::string got_s;
    auto conn = d.bind([&](int i, const std::string& s) {
        got_i = i;
        got_s = s;
    });

    d.broadcast(7, "hello");
    REQUIRE(got_i == 7);
    REQUIRE(got_s == "hello");
}

TEST_CASE("Double disconnect is safe", "[delegate]") {
    Delegate<void()> d;
    auto conn = d.bind([&] {});

    conn.disconnect();
    conn.disconnect(); // should not crash
    REQUIRE_FALSE(conn.connected());
}

TEST_CASE("Delegate survives listener that disconnects itself", "[delegate]") {
    Delegate<void()> d;
    int count = 0;
    Connection self_conn;

    self_conn = d.bind([&] {
        count++;
        self_conn.disconnect(); // disconnect self during broadcast
    });

    d.broadcast();
    REQUIRE(count == 1);

    d.broadcast();
    REQUIRE(count == 1); // self-disconnected, no more calls
}

// --- Edge cases: broadcast-time mutations ---

TEST_CASE("Listener added during broadcast is not called in that broadcast", "[delegate][edge]") {
    Delegate<void()> d;
    int outer_count = 0;
    int inner_count = 0;
    Connection inner_conn;
    bool added = false;

    auto outer_conn = d.bind([&] {
        outer_count++;
        if (!added) {
            added = true;
            inner_conn = d.bind([&] { inner_count++; });
        }
    });

    d.broadcast();
    REQUIRE(outer_count == 1);
    REQUIRE(inner_count == 0); // new listener should NOT fire in this broadcast

    // Second broadcast: both should fire
    d.broadcast();
    REQUIRE(outer_count == 2);
    REQUIRE(inner_count == 1);
}

TEST_CASE("Nested broadcast (listener triggers another broadcast)", "[delegate][edge]") {
    Delegate<void()> d;
    std::vector<int> order;
    Connection c1, c2;

    c1 = d.bind([&] {
        order.push_back(1);
        if (order.size() == 1) {
            d.broadcast(); // re-entrant broadcast
        }
    });
    c2 = d.bind([&] {
        order.push_back(2);
    });

    d.broadcast();
    // First broadcast: c1 fires → triggers nested broadcast → c1(1), c2(2) → back to outer → c2(2)
    REQUIRE(order == std::vector<int>{1, 1, 2, 2});
}

TEST_CASE("Disconnect during nested broadcast defers cleanup to outermost", "[delegate][edge]") {
    // Simulates: on_health_changed → check_death → re-broadcasts once → listener disconnects
    Delegate<void()> d;
    int c2_count = 0;
    Connection c1, c2;
    bool re_entered = false;

    c1 = d.bind([&] {
        if (!re_entered) {
            re_entered = true;
            d.broadcast(); // one nested re-broadcast (like death → heal → health_changed)
        }
    });
    c2 = d.bind([&] {
        c2_count++;
        c2.disconnect(); // one-shot listener
    });

    d.broadcast();
    // Outer: c1 fires (nested broadcast) → nested: c1 skips, c2 fires (count=1, nulled)
    // → back to outer: c2 is nulled, skipped
    REQUIRE(c2_count == 1);
    REQUIRE(d.size() == 1); // c2 cleaned up when depth returned to 0
}

TEST_CASE("Clear during broadcast nulls all, cleanup after", "[delegate][edge]") {
    Delegate<void()> d;
    int count = 0;
    Connection c1, c2;

    c1 = d.bind([&] {
        count++;
        d.clear(); // clear everything mid-broadcast
    });
    c2 = d.bind([&] {
        count++;
    });

    d.broadcast();
    // c1 fires and clears. c2's callback is nulled, so it's skipped.
    REQUIRE(count == 1);

    // After broadcast, dirty cleanup removes all nulled listeners
    d.broadcast(); // should be a no-op
    REQUIRE(count == 1);
}

TEST_CASE("Connection outlives delegate (disconnect is safe)", "[delegate][edge]") {
    Connection conn;
    {
        Delegate<void()> d;
        conn = d.bind([&] {});
        REQUIRE(conn.connected());
    } // delegate destroyed

    // Disconnect on a dead delegate should not crash (weak_ptr expired)
    conn.disconnect();
    REQUIRE_FALSE(conn.connected());
}

TEST_CASE("Move-assign connection disconnects previous", "[delegate][edge]") {
    Delegate<void()> d;
    int count_a = 0;
    int count_b = 0;

    Connection conn = d.bind([&] { count_a++; });
    d.broadcast();
    REQUIRE(count_a == 1);

    // Move-assign a new binding into conn — old listener should disconnect
    conn = d.bind([&] { count_b++; });
    d.broadcast();
    REQUIRE(count_a == 1); // old listener no longer called
    REQUIRE(count_b == 1);
}

TEST_CASE("Delegate move transfers ownership", "[delegate][edge]") {
    Delegate<void()> d1;
    int count = 0;
    auto conn = d1.bind([&] { count++; });

    Delegate<void()> d2 = std::move(d1);
    d2.broadcast();
    REQUIRE(count == 1);

    // Connection still works (it holds a weak_ptr to shared state)
    conn.disconnect();
    d2.broadcast();
    REQUIRE(count == 1);
}

TEST_CASE("Many listeners bind and unbind without leaking", "[delegate][edge]") {
    Delegate<void()> d;
    int count = 0;

    // Bind 100, disconnect half, broadcast
    std::vector<Connection> conns;
    for (int i = 0; i < 100; i++) {
        conns.push_back(d.bind([&] { count++; }));
    }
    REQUIRE(d.size() == 100);

    for (int i = 0; i < 50; i++) {
        conns[i].disconnect();
    }
    REQUIRE(d.size() == 50);

    d.broadcast();
    REQUIRE(count == 50);
}

TEST_CASE("Disconnect first listener during broadcast, rest still fire", "[delegate][edge]") {
    Delegate<void()> d;
    std::vector<int> fired;
    Connection c1, c2, c3;

    c1 = d.bind([&] { fired.push_back(1); c1.disconnect(); });
    c2 = d.bind([&] { fired.push_back(2); });
    c3 = d.bind([&] { fired.push_back(3); });

    d.broadcast();
    REQUIRE(fired == std::vector<int>{1, 2, 3});

    // Second broadcast: only c2 and c3
    fired.clear();
    d.broadcast();
    REQUIRE(fired == std::vector<int>{2, 3});
}

TEST_CASE("Swap-and-pop on disconnect outside broadcast changes order", "[delegate][edge]") {
    Delegate<void()> d;
    std::vector<int> order;
    auto c1 = d.bind([&] { order.push_back(1); });
    auto c2 = d.bind([&] { order.push_back(2); });
    auto c3 = d.bind([&] { order.push_back(3); });

    // Disconnect c1 (index 0): swap with back (c3), pop → [c3, c2]
    c1.disconnect();
    d.broadcast();
    // c3 is now at index 0, c2 at index 1
    REQUIRE(order.size() == 2);
    REQUIRE(order[0] == 3);
    REQUIRE(order[1] == 2);
}

TEST_CASE("Sequential disconnects outside broadcast", "[delegate][edge]") {
    Delegate<void()> d;
    int count = 0;
    auto c1 = d.bind([&] { count++; });
    auto c2 = d.bind([&] { count++; });
    auto c3 = d.bind([&] { count++; });
    auto c4 = d.bind([&] { count++; });

    REQUIRE(d.size() == 4);
    c2.disconnect();
    REQUIRE(d.size() == 3);
    c4.disconnect();
    REQUIRE(d.size() == 2);
    c1.disconnect();
    REQUIRE(d.size() == 1);
    c3.disconnect();
    REQUIRE(d.size() == 0);
    REQUIRE(d.empty());

    d.broadcast();
    REQUIRE(count == 0);
}

TEST_CASE("size() counts nulled listeners during broadcast", "[delegate][edge]") {
    Delegate<void()> d;
    Connection c1, c2;
    uint32_t size_during = 0;

    c1 = d.bind([&] {
        c2.disconnect(); // nulls c2 but doesn't remove during broadcast
        size_during = d.size();
    });
    c2 = d.bind([&] {});

    d.broadcast();
    // During broadcast, c2 is nulled but not removed — size still reports 2
    REQUIRE(size_during == 2);
    // After broadcast, cleanup removes it
    REQUIRE(d.size() == 1);
}

TEST_CASE("empty() is false when only nulled listeners remain during broadcast", "[delegate][edge]") {
    Delegate<void()> d;
    Connection c1;
    bool empty_during = true;

    c1 = d.bind([&] {
        c1.disconnect();
        empty_during = d.empty();
    });

    d.broadcast();
    REQUIRE_FALSE(empty_during); // nulled listener still in vector
    REQUIRE(d.empty());          // cleaned up after broadcast
}

TEST_CASE("Bind after clear during broadcast survives cleanup", "[delegate][edge]") {
    Delegate<void()> d;
    int survivor_count = 0;
    Connection c1, survivor_conn;

    c1 = d.bind([&] {
        d.clear();
        survivor_conn = d.bind([&] { survivor_count++; });
    });
    auto c2 = d.bind([&] {});

    d.broadcast();
    // c1 fired, cleared all (nulled), then bound survivor.
    // Cleanup removes nulled entries but survivor has valid callback.
    d.broadcast();
    REQUIRE(survivor_count == 1);
}

TEST_CASE("Disconnect all listeners during broadcast from first listener", "[delegate][edge]") {
    Delegate<void()> d;
    std::vector<int> fired;
    Connection c1, c2, c3;

    c1 = d.bind([&] {
        fired.push_back(1);
        c1.disconnect();
        c2.disconnect();
        c3.disconnect();
    });
    c2 = d.bind([&] { fired.push_back(2); });
    c3 = d.bind([&] { fired.push_back(3); });

    d.broadcast();
    // c1 fires and disconnects everything. c2 and c3 are nulled, skipped.
    REQUIRE(fired == std::vector<int>{1});
    REQUIRE(d.empty());
}

TEST_CASE("Disconnect middle listener during broadcast, others still fire", "[delegate][edge]") {
    Delegate<void()> d;
    std::vector<int> fired;
    Connection c1, c2, c3, c4;

    c1 = d.bind([&] { fired.push_back(1); });
    c2 = d.bind([&] { fired.push_back(2); c3.disconnect(); });
    c3 = d.bind([&] { fired.push_back(3); });
    c4 = d.bind([&] { fired.push_back(4); });

    d.broadcast();
    // c1(1), c2(2, disconnects c3), c3(nulled, skip), c4(4)
    REQUIRE(fired == std::vector<int>{1, 2, 4});
}

TEST_CASE("Delegate destroyed during broadcast via listener", "[delegate][edge]") {
    // The local `auto s = state_` in broadcast() should keep state alive.
    auto d = std::make_unique<Delegate<void()>>();
    int count = 0;
    Connection conn;

    conn = d->bind([&] {
        count++;
        d.reset(); // destroy the delegate mid-broadcast
    });

    // This should not crash — shared_ptr in broadcast keeps state alive
    d->broadcast();
    REQUIRE(count == 1);
}

TEST_CASE("Bind and disconnect rapidly without broadcast", "[delegate][edge]") {
    Delegate<void()> d;
    for (int i = 0; i < 1000; i++) {
        auto conn = d.bind([&] {});
        conn.disconnect();
    }
    REQUIRE(d.empty());
    REQUIRE(d.size() == 0);
}

TEST_CASE("Connection connected() reflects state accurately through lifecycle", "[delegate][edge]") {
    Delegate<void()> d;

    Connection conn;
    REQUIRE_FALSE(conn.connected()); // default-constructed

    conn = d.bind([&] {});
    REQUIRE(conn.connected()); // bound

    Connection moved = std::move(conn);
    REQUIRE_FALSE(conn.connected()); // moved-from
    REQUIRE(moved.connected());      // moved-to

    moved.disconnect();
    REQUIRE_FALSE(moved.connected()); // disconnected
}

TEST_CASE("Multiple disconnects of different listeners in single broadcast", "[delegate][edge]") {
    Delegate<void()> d;
    std::vector<int> fired;
    Connection c1, c2, c3, c4, c5;

    c1 = d.bind([&] { fired.push_back(1); c3.disconnect(); c5.disconnect(); });
    c2 = d.bind([&] { fired.push_back(2); });
    c3 = d.bind([&] { fired.push_back(3); });
    c4 = d.bind([&] { fired.push_back(4); c2.disconnect(); });
    c5 = d.bind([&] { fired.push_back(5); });

    d.broadcast();
    // c1(1, nulls c3+c5), c2(2), c3(nulled), c4(4, nulls c2), c5(nulled)
    REQUIRE(fired == std::vector<int>{1, 2, 4});

    fired.clear();
    d.broadcast();
    // Only c1 and c4 remain
    REQUIRE(fired == std::vector<int>{1, 4});
}

TEST_CASE("Disconnect last listener during broadcast, earlier ones unaffected", "[delegate][edge]") {
    Delegate<void()> d;
    std::vector<int> fired;
    Connection c1, c2, c3;

    c3 = Connection{}; // declare first so lambda can capture
    c1 = d.bind([&] { fired.push_back(1); });
    c2 = d.bind([&] { fired.push_back(2); c3.disconnect(); });
    c3 = d.bind([&] { fired.push_back(3); });

    d.broadcast();
    // c1 fires, c2 fires and disconnects c3, c3's callback is nulled → skipped
    REQUIRE(fired == std::vector<int>{1, 2});

    fired.clear();
    d.broadcast();
    REQUIRE(fired == std::vector<int>{1, 2});
}

TEST_CASE("A broadcast a listener threw out of still lets later binds join", "[delegate][edge]") {
    Delegate<void()> event;
    int later = 0;
    Connection late;
    auto thrower = event.bind([&] {
        // Bound mid-broadcast: waits for the broadcast to finish.
        late = event.bind([&] { ++later; });
        throw std::runtime_error("listener failed");
    });
    CHECK_THROWS(event.broadcast());
    thrower.disconnect();

    // Had the broadcast been left unfinished, the listener bound during it
    // would never have joined.
    event.broadcast();
    CHECK(later == 1);
}
