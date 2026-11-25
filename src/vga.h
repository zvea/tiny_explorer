#pragma once

#include <map>
#include "fetsim.h"
#include "extractor.h"

// A VGA timing: visible area, front porch, sync pulse and back porch, in pixels per line and
// lines per frame.
struct VGATiming {
    int h_visible, h_front, h_sync, h_back;
    int v_visible, v_front, v_sync, v_back;
    int h_total() const { return h_visible + h_front + h_sync + h_back; }
    int v_total() const { return v_visible + v_front + v_sync + v_back; }
    bool operator==(const VGATiming&) const = default;
};

// VESA DMT modes with pixel clocks from 25 to 65 MHz. The first is assumed until the signal
// has been measured.
inline constexpr VGATiming VGA_MODES[] = {
    { 640, 16,  96,  48,   480,  10, 2, 33},  // 640x480 @ 60 Hz
    { 640, 24,  40, 128,   480,   9, 3, 28},  // 640x480 @ 72 Hz
    { 640, 16,  64, 120,   480,   1, 3, 16},  // 640x480 @ 75 Hz
    { 640, 56,  56,  80,   480,   1, 3, 25},  // 640x480 @ 85 Hz
    { 800, 24,  72, 128,   600,   1, 2, 22},  // 800x600 @ 56 Hz
    { 800, 40, 128,  88,   600,   1, 4, 23},  // 800x600 @ 60 Hz
    { 800, 56, 120,  64,   600,  37, 6, 23},  // 800x600 @ 72 Hz
    { 800, 16,  80, 160,   600,   1, 3, 21},  // 800x600 @ 75 Hz
    {1024, 24, 136, 160,   768,   3, 6, 29},  // 1024x768 @ 60 Hz
};

// One sync input. The pulse is whichever level is shorter, so no pulse ends until both levels
// have been seen.
struct VGASyncInput {
    bool level = true;    // level at the previous clock
    int run = 0;          // clocks at that level so far
    int len[2] = {0, 0};  // length of the last completed run of each level; 0 = not seen yet

    // Takes this clock's level; returns true if it ends a sync pulse.
    bool pulse_ended(bool now) {
        bool ended = false;
        if (now != level) {
            len[level] = run;
            run = 0;
            ended = len[0] && len[1] && level == (len[1] < len[0]);
            level = now;
        }
        ++run;
        return ended;
    }
};

// Monitor model: a line starts where a line sync pulse ends, and a frame where a frame sync
// pulse ends. The measured clocks per line and lines per frame pick a mode from VGA_MODES,
// allowing several clocks per pixel; only that mode's visible area is drawn. A signal that
// matches no mode is drawn whole, porches and sync pulses included.
struct VGASimulator {
    Circuit& circuit;

    std::map<std::string, int> name2id;
    int clk = -1, rst_n = -1, ena = -1;
    int in_pins[8] = {-1,-1,-1,-1,-1,-1,-1,-1}, out_pins[8] = {-1,-1,-1,-1,-1,-1,-1,-1};

    static constexpr int max_width = 1440, max_height = 768;
    static constexpr int max_clocks_per_pixel = 4;
    int width = 0, height = 0;  // drawn area
    int ray_x = 0, ray_y = 0;   // beam position in the drawn area; outside it during blanking
    std::vector<uint8_t> vga_buffer;

    // Lines drawn since the last clear_dirty(), so the page copies only those. dirty_full asks
    // for the whole drawn area, after the buffer was cleared.
    bool dirty_full = true;
    int dirty_y0 = 0, dirty_y1 = -1;
    void clear_dirty() { dirty_full = false; dirty_y0 = 0; dirty_y1 = -1; }

    VGASyncInput hsync_in, vsync_in;
    int clocks = 0;            // clocks since the last line sync pulse ended
    int lines = 0;             // line starts since the last frame sync pulse ended
    int h_total = 0, v_total = 0;  // measured clocks per line, lines per frame; 0 = not yet
    bool line_measurable = false;  // a line has started since restart, so the next start
    bool frame_measurable = false; // (of a line, of a frame) ends a whole one
    VGATiming timing{};
    int clocks_per_pixel = 1;

    VGASimulator(Circuit& c) : circuit(c) {
        vga_buffer.assign(max_width * max_height * 3, 0);
        restart();
    }

    // Puts the beam at the top left of the picture, where a design's counters start after
    // reset, and forgets the measured timing.
    void restart() {
        hsync_in = {};
        vsync_in = {};
        h_total = v_total = 0;
        line_measurable = frame_measurable = false;
        set_timing(VGA_MODES[0], 1);
        clocks = timing.h_back;
        lines = timing.v_back;
        ray_x = ray_y = 0;
    }

    void set_timing(const VGATiming& t, int k) {
        if (t == timing && k == clocks_per_pixel) return;
        timing = t;
        clocks_per_pixel = k;
        width = std::min(t.h_visible, max_width);
        height = std::min(t.v_visible, max_height);
        std::fill(vga_buffer.begin(), vga_buffer.end(), 0);
        dirty_full = true;
    }

    // Called at each frame start once a whole line and a whole frame have been measured.
    void identify_mode() {
        for (const VGATiming& m : VGA_MODES) {
            if (m.v_total() != v_total || h_total % m.h_total() != 0) continue;
            int k = h_total / m.h_total();
            if (k <= max_clocks_per_pixel) { set_timing(m, k); return; }
        }
        set_timing({h_total, 0, 0, 0, v_total, 0, 0, 0}, 1);
    }

    void init(const std::vector<CircuitExtractor::LabeledWire>& labels) {
        name2id.clear();
        for (const auto& lw : labels) {
            name2id[lw.name] = lw.id;
        }
        // Find essential pins
        clk = getPin("clk");
        rst_n = getPin("rst_n");
        ena = getPin("ena");
        
        for (int i = 0; i < 8; ++i) {
            out_pins[i] = getPin("uo_out[" + std::to_string(i) + "]");
            in_pins[i] = getPin("ui_in[" + std::to_string(i) + "]");
        }
    }

    void reset() {
        if (!isValid()) return;
        if (ena != -1) circuit.set_signal(ena, 1);
        circuit.set_signal(rst_n, 0);
        // TT-specific reset behavior
        if (in_pins[1] != -1) circuit.set_signal(in_pins[1], 1);
        if (in_pins[4] != -1) circuit.set_signal(in_pins[4], 1);
        for (int i = 0; i < 10; ++i) {
            circuit.set_signal(clk, 0); circuit.settle(200);
            circuit.set_signal(clk, 1); circuit.settle(200);
            vga_tick();
        }
        if (in_pins[1] != -1) circuit.set_signal(in_pins[1], 0);
        if (in_pins[4] != -1) circuit.set_signal(in_pins[4], 0);
        circuit.set_signal(clk, 0); circuit.settle(200);
        circuit.set_signal(rst_n, 1); circuit.settle(200);
        restart();
    }

    bool isValid() const { return clk != -1 && rst_n != -1; }

    void run(int max_ticks = 1000000) {
        if (!isValid()) return;
        
        printf("VGA Sim: clk=%d, rst_n=%d, outputs=[", clk, rst_n);
        for(int i=0; i<8; ++i) printf("%d%s", out_pins[i], i==7 ? "]\n" : ",");

        printf("Resetting...\n");
        reset();
        
        printf("Running simulation...\n");
        auto t_start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < max_ticks; ++i) {
            circuit.set_signal(clk, 0); circuit.settle(200);
            circuit.set_signal(clk, 1); circuit.settle(200);
            vga_tick();
            if ((i + 1) % 10000 == 0) {
                auto t_now = std::chrono::high_resolution_clock::now();
                double elapsed = std::chrono::duration<double>(t_now - t_start).count();
                uint8_t out = 0;
                for (int j=0; j<8; j++) {
                    if (out_pins[j] != -1 && (circuit.wire_data[out_pins[j]] & Circuit::V_MASK)) out |= (1 << j);
                }
                printf("  Tick %d (%.0f px/s), ray at %d,%d, out=%02x  shorts=%d\n",
                    i + 1, (i + 1) / elapsed, ray_x, ray_y, out, circuit.short_count);
                savePPM("vga.ppm");
            }
        }
        savePPM("vga.ppm");
    }

    int getPin(const std::string& name) {
        return name2id.count(name) ? name2id.at(name) : -1;
    }

    void settle(int max_waves=200) {
        circuit.settle(max_waves);
    }

    void vga_tick() {
        auto val = [&](int i) { return (out_pins[i] != -1) && (circuit.wire_data[out_pins[i]] & Circuit::V_MASK); };
        bool r1 = val(0), g1 = val(1), b1 = val(2), vsync = val(3);
        bool r0 = val(4), g0 = val(5), b0 = val(6), hsync = val(7);

        if (hsync_in.pulse_ended(hsync)) {
            h_total = line_measurable ? clocks : 0;
            line_measurable = true;
            clocks = 0;
            ++lines;
        }
        if (vsync_in.pulse_ended(vsync)) {
            if (frame_measurable && h_total) {
                v_total = lines;
                identify_mode();
            }
            frame_measurable = true;
            lines = 0;
        }

        const int k = clocks_per_pixel;
        const int px = clocks - timing.h_back * k;  // clocks into the visible line
        ray_x = px >= 0 ? px / k : -1;
        ray_y = lines - timing.v_back;
        if (px >= 0 && px % k == 0 && ray_x < width && ray_y >= 0 && ray_y < height) {
            int idx = (ray_y * max_width + ray_x) * 3;
            vga_buffer[idx + 0] = (r1 * 2 + r0) * 85;
            vga_buffer[idx + 1] = (g1 * 2 + g0) * 85;
            vga_buffer[idx + 2] = (b1 * 2 + b0) * 85;
            if (dirty_y0 > dirty_y1) dirty_y0 = dirty_y1 = ray_y;
            else { dirty_y0 = std::min(dirty_y0, ray_y); dirty_y1 = std::max(dirty_y1, ray_y); }
        }
        ++clocks;
    }

    void savePPM(const char* filename) {
        FILE* f = fopen(filename, "wb");
        if (!f) return;
        fprintf(f, "P6\n%d %d\n255\n", width, height);
        for (int i=0; i<height; ++i) {
            fwrite(vga_buffer.data() + i*max_width*3, 1, width*3, f);    
        }
        fclose(f);
    }
};
