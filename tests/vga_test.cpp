// Unit tests for the VGA monitor model in src/vga.h.
//
// Each test drives the model's eight output pins with a synthetic VGA signal, clock by clock,
// as a design would after reset (counters at the top left of the picture), then checks that
// the picture lands in the frame buffer at the right place and size.
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

#include "vga.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); \
    ++failures; } } while (0)

// One axis of a timing: visible, front porch, sync pulse, back porch (pixels or lines).
struct Axis { int visible, front, sync, back; int total() const { return visible + front + sync + back; } };

struct Signal {
    Axis h, v;
    bool h_active_high = false, v_active_high = false;
    int clocks_per_pixel = 1;
};

static const Signal VGA_640x480_60 = { {640, 16, 96, 48}, {480, 10, 2, 33} };
static const Signal VGA_800x600_72 = { {800, 56, 120, 64}, {600, 37, 6, 23}, true, true };

// A 6-bit colour (never black) that changes with both coordinates, so a misplaced picture
// does not match.
static int pattern(int x, int y) { return (x + 3 * y) % 63 + 1; }

struct Rig {
    Circuit circuit;
    VGASimulator vga{circuit};
    Rig() {
        circuit.wire_data.assign(8, 0);
        for (int i = 0; i < 8; ++i) vga.out_pins[i] = i;  // wire i drives uo_out[i]
    }

    // TinyVGA PMOD order: uo_out = {hsync, b0, g0, r0, vsync, b1, g1, r1}.
    void set_outputs(int colour, bool hsync, bool vsync) {
        int r = colour & 3, g = (colour >> 2) & 3, b = (colour >> 4) & 3;
        int bits[8] = { r >> 1, g >> 1, b >> 1, vsync, r & 1, g & 1, b & 1, hsync };
        for (int i = 0; i < 8; ++i) circuit.wire_data[i] = bits[i];
    }

    // Drives whole frames, starting where the previous call stopped.
    void run_frames(const Signal& s, int frames) {
        for (int f = 0; f < frames; ++f)
            for (int y = 0; y < s.v.total(); ++y)
                for (int x = 0; x < s.h.total(); ++x)
                    for (int k = 0; k < s.clocks_per_pixel; ++k) {
                        bool visible = x < s.h.visible && y < s.v.visible;
                        bool hpulse = x >= s.h.visible + s.h.front && x < s.h.visible + s.h.front + s.h.sync;
                        bool vpulse = y >= s.v.visible + s.v.front && y < s.v.visible + s.v.front + s.v.sync;
                        // Only a pixel's first clock carries its colour: the model must
                        // sample that one.
                        set_outputs(visible ? (k == 0 ? pattern(x, y) : 0) : 0,
                                    hpulse == s.h_active_high, vpulse == s.v_active_high);
                        vga.vga_tick();
                    }
    }

    int pixel(int x, int y) const {
        const uint8_t* p = &vga.vga_buffer[(y * vga.max_width + x) * 3];
        return (p[0] / 85) | ((p[1] / 85) << 2) | ((p[2] / 85) << 4);
    }

    // Checks that the buffer holds exactly the visible picture, at origin (x0, y0).
    void check_picture(const char* name, const Signal& s, int x0 = 0, int y0 = 0) {
        int bad = 0;
        for (int y = 0; y < s.v.visible; ++y)
            for (int x = 0; x < s.h.visible; ++x)
                if (pixel(x0 + x, y0 + y) != pattern(x, y) && bad++ == 0)
                    printf("  %s: first mismatch at picture (%d, %d): got %d, want %d\n",
                           name, x, y, pixel(x0 + x, y0 + y), pattern(x, y));
        CHECK(bad == 0, "%s: %d of %d picture pixels misplaced", name, bad, s.h.visible * s.v.visible);
    }

    void check_size(const char* name, int width, int height) {
        CHECK(vga.width == width && vga.height == height,
              "%s: frame size %dx%d, want %dx%d", name, vga.width, vga.height, width, height);
    }
};

static void test_640x480_active_low() {
    Rig rig;
    rig.run_frames(VGA_640x480_60, 1);
    rig.check_size("640x480 active-low, frame 1", 640, 480);
    rig.check_picture("640x480 active-low, frame 1", VGA_640x480_60);
    rig.run_frames(VGA_640x480_60, 2);
    rig.check_size("640x480 active-low, frame 3", 640, 480);
    rig.check_picture("640x480 active-low, frame 3", VGA_640x480_60);
}

static void test_640x480_active_high() {
    Signal s = VGA_640x480_60;
    s.h_active_high = s.v_active_high = true;
    Rig rig;
    rig.run_frames(s, 1);
    rig.check_size("640x480 active-high, frame 1", 640, 480);
    rig.check_picture("640x480 active-high, frame 1", s);
    rig.run_frames(s, 1);
    rig.check_size("640x480 active-high, frame 2", 640, 480);
    rig.check_picture("640x480 active-high, frame 2", s);
}

static void test_800x600_72() {
    Rig rig;
    rig.run_frames(VGA_800x600_72, 3);
    rig.check_size("800x600@72", 800, 600);
    rig.check_picture("800x600@72", VGA_800x600_72);
}

static void test_640x480_two_clocks_per_pixel() {
    Signal s = VGA_640x480_60;
    s.clocks_per_pixel = 2;
    Rig rig;
    rig.run_frames(s, 3);
    rig.check_size("640x480, 2 clocks per pixel", 640, 480);
    rig.check_picture("640x480, 2 clocks per pixel", s);
}

// Timings that match no standard mode: the model shows the whole raster, each line starting
// where the line sync pulse ends and each frame where the frame sync pulse ends.
static void test_unknown_timing() {
    const Signal s = { {100, 10, 20, 30}, {50, 3, 2, 5} };
    const int h_sync_end = s.h.visible + s.h.front + s.h.sync;
    const int v_sync_end = s.v.visible + s.v.front + s.v.sync;
    Rig rig;
    rig.run_frames(s, 3);
    rig.check_size("unknown timing", s.h.total(), s.v.total());
    rig.check_picture("unknown timing", s, s.h.total() - h_sync_end, s.v.total() - v_sync_end);
}

int main() {
    test_640x480_active_low();
    test_640x480_active_high();
    test_800x600_72();
    test_640x480_two_clocks_per_pixel();
    test_unknown_timing();
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("all VGA tests passed\n");
    return 0;
}
