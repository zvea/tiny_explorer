#pragma once

#include "fetsim.h"
#include "vga.h"

// Advances the simulation by steps: one settling wave while the circuit is unsettled,
// otherwise, with the automatic clock on, one toggle of the clock. The VGA model samples the
// outputs once the circuit has settled after a rising clock edge.
//
// run() takes many steps per call, so the web worker need not call into WASM once per wave.
struct SimStepper {
    int clk = -1;                   // clock wire, or -1 if the design has none
    int last_clk = 0;               // clock level after the last toggle by run()
    bool pending_vga_tick = false;  // a rising edge happened; sample once settled
    uint32_t clock_count = 0;       // rising edges made by run(), for the speed readout; wraps

    void set_clock(int wire) {
        clk = wire;
        last_clk = 0;
        pending_vga_tick = false;
    }

    // Takes up to max_steps steps; returns how many it took. Stops early when the circuit is
    // settled and there is no clock to toggle.
    int run(Circuit& c, VGASimulator& vga, int max_steps, bool auto_clock) {
        int done = 0;
        for (; done < max_steps; ++done) {
            if (!c.is_settled()) {
                c.run_wave();
            } else if (auto_clock && clk != -1) {
                c.set_signal(clk, (c.wire_data[clk] & Circuit::V_MASK) ? 0 : 1);
                int now = c.wire_data[clk] & Circuit::V_MASK;
                if (now == 1 && last_clk == 0) {
                    pending_vga_tick = true;
                    ++clock_count;
                }
                last_clk = now;
            } else {
                break;
            }
            if (pending_vga_tick && c.is_settled()) {
                vga.vga_tick();
                pending_vga_tick = false;
            }
        }
        return done;
    }
};
