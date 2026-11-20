#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r::GCodeSmoothing {

// Native C++ port of Layer Time Smooth V5.2.1.
// Feedrate-only transformation: XYZ and extrusion coordinates are never rewritten.
struct LayerTimeSmoothConfig {
    bool enabled = true;
    double max_layer_variation_percent = 25.0;
    double max_slowdown_percent = 30.0;
    double min_speed_mm_s = 18.0;
    double max_total_time_increase_percent = 15.0;
    bool preserve_outer_walls = true;
    bool adjust_inner_walls = true;
    bool adjust_infill = true;
    bool adjust_top_surface = true;
    bool adjust_support = false;
    bool adjust_bridges = false;
    bool adjust_gap_fill = false;
    bool write_layer_report = true;
    double quality_speed = 0.50;
    int smooth_radius = 5;
    bool keep_min_speed = true;
    std::vector<double> manual_profile = std::vector<double>(64, 1.0);
};

struct LayerTimeSmoothReport {
    enum class Status {
        Disabled, AlreadyProcessed, NoLayers, NoChange, Processed, Failed
    };

    Status status = Status::NoChange;
    std::size_t layers = 0;
    std::size_t changed_moves = 0;
    std::size_t restored_moves = 0;
    double time_before = 0.0;
    double time_after = 0.0;
    double max_jump_before = 0.0;
    double max_jump_after = 0.0;
    double effective_layer_ratio = 0.0;
    std::vector<double> scale_profile;
    std::string message;
};

class LayerTimeSmooth {
public:
    // Processes G-code in memory. On failure or a safe-skip condition, gcode is
    // left byte-for-byte unchanged. Only F words on eligible extrusion moves
    // are changed; travel feedrates are restored when modal feed would leak.
    static bool process(std::string& gcode,
                        const LayerTimeSmoothConfig& config,
                        LayerTimeSmoothReport* report = nullptr);
};

} // namespace Slic3r::GCodeSmoothing
