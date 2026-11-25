// Holds the switch-level engine (src/fetsim.h) to a frozen copy of its original version, with
// the corrections listed there (tests/reference/fetsim_reference.h), so that changes made for
// speed cannot change results.
//
// Random transistor circuits, shorts, floating wires and feedback included, are built with both
// engines and driven alike by random input toggles. After every wave the two must agree on
// every wire's value, on the work queue's contents and order, and on the count of shorts.
//
// Build and run: make test

// The project's headers expect what src/main.cpp includes before them.
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include <gdstk/gdstk.hpp>

#include "fetsim.h"
#include "reference/fetsim_reference.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); \
    ++failures; } } while (0)

struct Fet { int gate, t0, t1, type; };

template <class Builder>
static auto build(const std::vector<Fet>& fets) {
    Builder b;
    for (const Fet& f : fets) b.add_fet(f.gate, f.t0, f.t1, f.type, -1);
    return b.build();
}

// The work queue's pending wires, in order.
template <class Queue>
static std::vector<uint32_t> pending(const Queue& q) {
    std::vector<uint32_t> v;
    const size_t mask = q.data.size() - 1;
    for (size_t i = q.head; i != q.tail; i = (i + 1) & mask) v.push_back(q.data[i]);
    return v;
}

static bool same_state(const reference::Circuit& r, const Circuit& c) {
    if (r.wire_data.size() != c.wire_data.size() || r.short_count != c.short_count) return false;
    for (size_t i = 0; i < r.wire_data.size(); ++i)
        if ((r.wire_data[i] & 1) != (c.wire_data[i] & 1)) return false;
    return pending(r.dirty_wires) == pending(c.dirty_wires);
}

// A random circuit: wires 0 and 1 are the rails, then inputs (set from outside, as the viewer
// may set any wire), then internal wires. Gates are mostly inputs or internal wires, sometimes
// a rail (as with tie cells); terminals are mostly rails or internal wires, sometimes an input.
static std::vector<Fet> random_circuit(std::mt19937& rng, std::vector<int>& inputs) {
    const int n_inputs = 2 + rng() % 5, n_internal = 4 + rng() % 40;
    inputs.clear();
    for (int i = 0; i < n_inputs; ++i) inputs.push_back(2 + i);
    const int first_internal = 2 + n_inputs, n_gates = n_inputs + n_internal;
    auto terminal = [&] {
        const int r = rng() % 20;
        if (r < 6) return int(rng() % 2);
        if (r < 8) return 2 + int(rng() % n_inputs);
        return first_internal + int(rng() % n_internal);
    };
    std::vector<Fet> fets(n_internal * (1 + rng() % 3));
    for (Fet& f : fets) {
        f.gate = rng() % 20 == 0 ? int(rng() % 2) : 2 + int(rng() % n_gates);
        do { f.t0 = terminal(); f.t1 = terminal(); } while (f.t0 == f.t1);
        f.type = rng() % 2;
    }
    return fets;
}

static void test_random_circuits() {
    long waves = 0;
    for (unsigned seed = 1; seed <= 1000 && failures == 0; ++seed) {
        std::mt19937 rng(seed);
        std::vector<int> inputs;
        const std::vector<Fet> fets = random_circuit(rng, inputs);
        reference::Circuit r = build<reference::CircuitBuilder>(fets);
        Circuit c = build<CircuitBuilder>(fets);
        CHECK(same_state(r, c), "circuit %u: differs after building", seed);
        for (int toggle = 0; toggle < 40 && failures == 0; ++toggle) {
            const int wire = inputs[rng() % inputs.size()];
            if (wire >= (int)r.wire_n()) continue;  // an input no transistor uses
            const uint8_t value = rng() % 2;
            r.set_signal(wire, value);
            c.set_signal(wire, value);
            CHECK(same_state(r, c), "circuit %u, toggle %d: differs after setting the input", seed, toggle);
            // Some random circuits oscillate, and in the original engine their work queue can
            // grow without bound (stale entries pile up); stop following those.
            const size_t queue_limit = 64 * r.wire_n();
            for (int wave = 0; wave < 200 && !r.is_settled() && r.dirty_wires.size() < queue_limit
                               && failures == 0; ++wave, ++waves) {
                r.run_wave();
                c.run_wave();
                CHECK(same_state(r, c), "circuit %u, toggle %d, wave %d: differs", seed, toggle, wave);
            }
        }
    }
    CHECK(waves > 10000, "only %ld waves compared", waves);
    printf("engine matches the reference over %ld waves\n", waves);
}

int main() {
    test_random_circuits();
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("all engine equivalence tests passed\n");
    return 0;
}
