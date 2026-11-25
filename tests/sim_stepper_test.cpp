// Unit tests for SimStepper in src/sim_stepper.h.
//
// The web worker used to drive the simulation one step at a time from JavaScript: a settling
// wave if the circuit is unsettled, otherwise (with the automatic clock on) a clock toggle,
// with the VGA model sampled once the circuit settles after a rising edge. SimStepper does the
// same in C++, many steps per call. These tests run small transistor circuits both ways and
// require identical states after every call, for many chunk sizes.
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

#include "sim_stepper.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); \
    ++failures; } } while (0)

enum { GND = 0, VPWR = 1 };

static void add_inverter(CircuitBuilder& b, int in, int out) {
    b.add_fet(in, GND, out, FET::N, -1);
    b.add_fet(in, VPWR, out, FET::P, -1);
}

// The worker's former JavaScript loop, step for step (web/gds_worker.js before SimStepper).
struct ReferenceLoop {
    int clk;
    int last_clk = 0;
    bool pending_vga_tick = false;
    uint32_t rising_edges = 0;  // not in the worker loop: counted here to check SimStepper's count

    int run(Circuit& c, VGASimulator& vga, int iterations, bool auto_clock) {
        int done = 0;
        while (iterations > 0) {
            if (!c.is_settled()) {
                c.run_wave();
                if (c.is_settled() && pending_vga_tick) {
                    vga.vga_tick();
                    pending_vga_tick = false;
                }
            } else if (auto_clock && clk != -1) {
                int val = c.wire_data[clk] & 1;
                c.set_signal(clk, val ? 0 : 1);
                int now = c.wire_data[clk] & 1;
                if (now == 1 && last_clk == 0) { pending_vga_tick = true; ++rising_edges; }
                last_clk = now;
                if (c.is_settled() && pending_vga_tick) {
                    vga.vga_tick();
                    pending_vga_tick = false;
                }
            } else {
                break;
            }
            --iterations;
            ++done;
        }
        return done;
    }
};

// Two copies of one circuit: one driven by the reference loop, one by SimStepper.
struct Pair {
    Circuit ref_c, new_c;
    VGASimulator ref_vga{ref_c}, new_vga{new_c};
    ReferenceLoop ref;
    SimStepper stepper;

    Pair(CircuitBuilder& b, int clk, int hsync_wire) : ref_c(b.build()), new_c(b.build()), ref{clk} {
        ref_vga.out_pins[7] = new_vga.out_pins[7] = hsync_wire;
        stepper.set_clock(clk);
    }

    void check_same(const char* name, int step) {
        CHECK(ref_c.wire_data == new_c.wire_data, "%s, call %d: wire states differ", name, step);
        CHECK(ref_c.dirty_wires.size() == new_c.dirty_wires.size(),
              "%s, call %d: %zu vs %zu dirty wires", name, step,
              ref_c.dirty_wires.size(), new_c.dirty_wires.size());
        CHECK(ref_vga.clocks == new_vga.clocks && ref_vga.lines == new_vga.lines,
              "%s, call %d: VGA model sampled differently", name, step);
        CHECK(ref.last_clk == stepper.last_clk && ref.pending_vga_tick == stepper.pending_vga_tick,
              "%s, call %d: clock edge state differs", name, step);
    }
};

// A chain of five inverters from the clock: each edge takes several waves to settle.
static CircuitBuilder inverter_chain() {
    CircuitBuilder b;
    for (int w = 2; w < 7; ++w) add_inverter(b, w, w + 1);  // clk = 2, output = 7
    return b;
}

static void test_matches_reference_for_any_chunk_size() {
    for (int chunk : {1, 2, 3, 5, 7, 11, 64}) {
        CircuitBuilder b = inverter_chain();
        Pair p(b, 2, 7);
        for (int call = 0; call < 200; ++call) {
            int a = p.ref.run(p.ref_c, p.ref_vga, chunk, true);
            int n = p.stepper.run(p.new_c, p.new_vga, chunk, true);
            CHECK(a == n, "chunk %d, call %d: %d vs %d steps done", chunk, call, a, n);
            p.check_same("inverter chain", call);
        }
        CHECK(p.new_vga.clocks > 0, "chunk %d: the VGA model was never sampled", chunk);
        CHECK(p.stepper.clock_count == p.ref.rising_edges && p.ref.rising_edges > 0,
              "chunk %d: counted %u clocks, want %u", chunk, p.stepper.clock_count, p.ref.rising_edges);
    }
}

static void test_stops_when_settled_without_clock() {
    CircuitBuilder b = inverter_chain();
    Pair p(b, 2, 7);
    p.stepper.run(p.new_c, p.new_vga, 1000, true);   // leaves the circuit mid-edge or settled
    p.stepper.run(p.new_c, p.new_vga, 1000, false);  // settles, then stops: no clock to toggle
    CHECK(p.new_c.is_settled(), "circuit not settled");
    CHECK(p.stepper.run(p.new_c, p.new_vga, 1000, false) == 0, "steps taken with nothing to do");
}

// Three inverters in a ring never settle, so the clock never toggles; every call uses its
// whole budget on waves.
static void test_never_settling_circuit_uses_whole_budget() {
    CircuitBuilder b;
    add_inverter(b, 3, 4);
    add_inverter(b, 4, 5);
    add_inverter(b, 5, 3);
    b.add_fet(2, GND, 6, FET::N, -1);  // the clock drives something, unrelated to the ring
    Pair p(b, 2, 6);
    p.new_c.set_signal(3, 1);  // start the ring
    p.ref_c.set_signal(3, 1);
    for (int call = 0; call < 50; ++call) {
        CHECK(p.stepper.run(p.new_c, p.new_vga, 9, true) == 9, "call %d: budget not used", call);
        p.ref.run(p.ref_c, p.ref_vga, 9, true);
        p.check_same("ring oscillator", call);
    }
    CHECK((p.new_c.wire_data[2] & 1) == 0, "clock toggled although the circuit never settled");
}

int main() {
    test_matches_reference_for_any_chunk_size();
    test_stops_when_settled_without_clock();
    test_never_settling_circuit_uses_whole_budget();
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("all stepper tests passed\n");
    return 0;
}
