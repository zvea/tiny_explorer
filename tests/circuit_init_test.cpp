// Unit tests for Circuit::init() in src/fetsim.h: once a newly built circuit has settled,
// every gate's output must match its inputs.
//
// init() gives the wires their first values one group at a time. A wire can get a value in one
// group and a different value in a later group. When that happens, the gates driven by that
// wire must be resolved again with its new value.
//
// Build and run: make test

// The project's headers expect what src/main.cpp includes before them.
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <gdstk/gdstk.hpp>

#include "fetsim.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); \
    ++failures; } } while (0)

enum { GND = 0, VPWR = 1 };

static void add_inverter(CircuitBuilder& b, int in, int out) {
    b.add_fet(in, GND, out, FET::N, -1);
    b.add_fet(in, VPWR, out, FET::P, -1);
}

// The circuit: Z is tied low, G = !Z, X = !G, and Y = !X. X's pull-down goes through a middle
// wire M and a transistor that is always on. The correct values are Z = 0, G = 1, X = 0, Y = 1.
//
// init() takes the wires in order of their numbers, which CircuitBuilder keeps. What it does:
//   1. X comes first. G has no value yet and reads 0, so X gets 1.
//   2. Y is resolved from X = 1 and gets 0. X already has a value, so Y is not queued.
//   3. G gets 1.
//   4. M is resolved next. It has no value until G does, so its group comes after Y's. With
//      G = 1, M and X form one group pulled to ground, so X gets 0.
// Y must then be resolved again from X = 0. Without that, Y keeps the 0 from step 2. X is
// queued from step 1, but when it is resolved again its value no longer changes, so that does
// not reach Y.
static void test_a_wire_changed_by_a_later_group_updates_its_gates() {
    enum { X = 2, Y = 3, G = 4, Z = 5, M = 6 };
    CircuitBuilder b;
    b.add_fet(G, X, VPWR, FET::P, -1);
    b.add_fet(G, X, M, FET::N, -1);
    b.add_fet(VPWR, M, GND, FET::N, -1);
    add_inverter(b, X, Y);
    add_inverter(b, Z, G);
    b.add_fet(VPWR, Z, GND, FET::N, -1);
    Circuit c = b.build();
    c.settle(100);
    auto v = [&](int w) { return c.wire_data[w] & Circuit::V_MASK; };
    CHECK(c.is_settled(), "the circuit does not settle");
    CHECK(v(Z) == 0 && v(G) == 1 && v(X) == 0, "Z, G, X = %d, %d, %d; expected 0, 1, 0", v(Z), v(G), v(X));
    CHECK(v(Y) == 1, "Y = %d with X = %d: the inverter on X kept the value X had before its group changed it", v(Y), v(X));
}

int main() {
    test_a_wire_changed_by_a_later_group_updates_its_gates();
    if (failures) { printf("circuit_init_test: %d failure(s)\n", failures); return 1; }
    printf("circuit_init_test: all passed\n");
    return 0;
}
