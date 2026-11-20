#include "../src/libslic3r/GCode/LayerTimeSmooth.hpp"

#include <cassert>
#include <iostream>
#include <string>

using Slic3r::GCode::LayerTimeSmooth;
using Slic3r::GCode::LayerTimeSmoothConfig;
using Slic3r::GCode::LayerTimeSmoothReport;

static std::string sample() {
    return "G21\nG90\nM83\nG92 X0 Y0 Z0 E0\n"
        ";LAYER_CHANGE\n;TYPE:Inner wall\nG1 X60 Y0 E2 F3600\n"
        ";LAYER_CHANGE\n;TYPE:Inner wall\nG1 X1260 Y0 E2 F3600\n"
        ";LAYER_CHANGE\n;TYPE:Inner wall\nG1 X1320 Y0 E2 F3600\n"
        ";TYPE:Travel\nG0 X1330 Y0\n";
}
int main() {
    LayerTimeSmoothConfig c;
    c.min_speed_mm_s=1;
    c.max_slowdown_percent=60;
    c.max_total_time_increase_percent=50;
    c.smooth_radius=1;
    LayerTimeSmoothReport report;
    auto gcode=sample();
    const bool changed=LayerTimeSmooth::process(gcode,c,&report);
    assert(changed);
    assert(report.status==LayerTimeSmoothReport::Status::Processed);
    assert(report.layers==3);
    assert(report.changed_moves>0);
    assert(gcode.find("G1 X60 Y0 E2 F")!=std::string::npos);
    assert(gcode.find("G0 X1330 Y0 F3600.0")!=std::string::npos);
    assert(gcode.find("; LAYER_TIME_SMOOTH_V521_NATIVE")!=std::string::npos);
    const std::string once=gcode;
    assert(!LayerTimeSmooth::process(gcode,c,&report));
    assert(gcode==once);

    c.enabled=false;
    auto original=sample();
    const std::string before=original;
    assert(!LayerTimeSmooth::process(original,c,&report));
    assert(original==before);

    auto no_layers=std::string("G1 X10 E1 F1200\n");
    const auto no_layers_before=no_layers;
    c.enabled=true;
    assert(!LayerTimeSmooth::process(no_layers,c,&report));
    assert(no_layers==no_layers_before);
    std::cout<<"Layer Time Smooth native unit tests passed\n";
}
