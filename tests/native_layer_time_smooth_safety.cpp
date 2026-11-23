#include "../src/libslic3r/GCode/LayerTimeSmooth.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

using Slic3r::GCodeSmoothing::LayerTimeSmooth;
using Slic3r::GCodeSmoothing::LayerTimeSmoothConfig;
using Slic3r::GCodeSmoothing::LayerTimeSmoothReport;

static std::string layers(const std::string& role) {
    return "G21\nG94\nG90\nM83\nG92 X0 Y0 Z0 E0\n"
        ";LAYER_CHANGE\n;TYPE:" + role + "\nG1 X60 Y0 E2 F3600\n"
        ";LAYER_CHANGE\n;TYPE:" + role + "\nG1 X1260 Y0 E2 F3600\n"
        ";LAYER_CHANGE\n;TYPE:" + role + "\nG1 X1320 Y0 E2 F3600\n"
        ";TYPE:Travel\nG0 X1330 Y0\n";
}

static LayerTimeSmoothConfig settings() {
    LayerTimeSmoothConfig c;
    c.max_layer_variation_percent = 25;
    c.max_slowdown_percent = 60;
    c.min_speed_mm_s = 1;
    c.max_total_time_increase_percent = 50;
    c.quality_speed = 1;
    c.smooth_radius = 1;
    return c;
}

static bool changed_with(const std::string& role, const LayerTimeSmoothConfig& config) {
    std::string gcode = layers(role);
    LayerTimeSmoothReport report;
    const bool changed = LayerTimeSmooth::process(gcode, config, &report);
    assert(changed == (report.status == LayerTimeSmoothReport::Status::Processed));
    return changed;
}

int main() {
    LayerTimeSmoothReport report;

    // Protected outer walls: untouched by default, opt-in allows smoothing.
    auto c = settings();
    assert(!changed_with("Outer wall", c));
    c.preserve_outer_walls = false;
    assert(changed_with("Outer wall", c));

    // Adjustability for each feature is independent.
    c = settings();
    assert(changed_with("Inner wall", c));
    c.adjust_inner_walls = false;
    assert(!changed_with("Inner wall", c));
    c = settings();
    assert(changed_with("Sparse infill", c));
    c.adjust_infill = false;
    assert(!changed_with("Sparse infill", c));
    c = settings();
    assert(changed_with("Top surface", c));
    c.adjust_top_surface = false;
    assert(!changed_with("Top surface", c));
    c = settings();
    assert(!changed_with("Support material", c));
    c.adjust_support = true;
    assert(changed_with("Support material", c));
    c = settings();
    assert(!changed_with("Bridge infill", c));
    c.adjust_bridges = true;
    assert(changed_with("Bridge infill", c));

    // Enforce no slowdown and enforce an original-speed floor.
    c = settings();
    c.max_slowdown_percent = 0;
    assert(!changed_with("Inner wall", c));
    c = settings();
    c.min_speed_mm_s = 60; // all movements start at exactly 60 mm/s
    assert(!changed_with("Inner wall", c));

    // A transformed file is idempotent and obeys the total-time budget.
    c = settings();
    std::string gcode = layers("Inner wall");
    assert(LayerTimeSmooth::process(gcode, c, &report));
    assert(report.layers == 3);
    assert(report.changed_moves > 0);
    assert(report.time_after >= report.time_before);
    assert(report.time_after <= report.time_before * 1.50 + 0.01);
    const std::string processed = gcode;
    assert(!LayerTimeSmooth::process(gcode, c, &report));
    assert(report.status == LayerTimeSmoothReport::Status::AlreadyProcessed);
    assert(gcode == processed);

    // Feed mode G93 (inverse-time) or G95 (per-revolution) is unsafe to
    // interpret as mm/min. Such input must remain byte-for-byte untouched.
    for (const std::string mode : {"G93", "G95"}) {
        std::string unsafe = mode + "\n" + layers("Inner wall");
        const std::string before = unsafe;
        assert(!LayerTimeSmooth::process(unsafe, c, &report));
        assert(report.status == LayerTimeSmoothReport::Status::Failed);
        assert(unsafe == before);
    }

    // CRLF lines must stay CRLF, including injected reports and restored feed.
    std::string windows = layers("Inner wall");
    for (std::size_t i = 0; i < windows.size(); ++i)
        if (windows[i] == '\n') { windows.insert(i, 1, '\r'); ++i; }
    assert(LayerTimeSmooth::process(windows, c, &report));
    assert(windows.find("\r\r\n") == std::string::npos);
    assert(windows.find("; LAYER_TIME_SMOOTH_V5_PROCESSED=1\r\n") != std::string::npos);

    std::cout << "Layer Time Smooth safety and feature tests passed\n";
}
