#include "LayerTimeSmooth.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <numeric>
#include <iomanip>
#include <regex>
#include <sstream>
#include <utility>

namespace Slic3r::GCodeSmoothing {
namespace {
struct LayerStats {
    double total = 0.0;
    double fixed = 0.0;
    double adjustable = 0.0;
    double max_adjustable_time = 0.0;
    std::size_t eligible = 0;
    std::vector<std::pair<double, double>> eligible_floors;
};
struct LayerSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
};
struct Analysis {
    std::vector<LayerSpan> spans;
    std::vector<LayerStats> stats;
    std::vector<double> times;
    std::vector<int> line_to_layer;
};

double clampv(double value, double lo, double hi)
{
    return std::max(lo, std::min(hi, value));
}

std::string trim(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(),
        [](unsigned char c) { return std::isspace(c) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
        [](unsigned char c) { return std::isspace(c) != 0; }).base();
    if (first >= last)
        return {};
    return std::string(first, last);
}

std::string lower_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string code_part(const std::string& line)
{
    return line.substr(0, line.find(';'));
}

std::string comment_part(const std::string& line)
{
    const auto pos = line.find(';');
    return pos == std::string::npos ? std::string() : line.substr(pos);
}

std::string command(const std::string& code)
{
    std::istringstream input(trim(code));
    std::string result;
    input >> result;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

std::map<char, double> words(const std::string& code)
{
    static const std::regex word_re(R"(([A-Za-z])\s*(-?(?:\d+(?:\.\d*)?|\.\d+)))");
    std::map<char, double> result;
    for (std::sregex_iterator it(code.begin(), code.end(), word_re), end; it != end; ++it) {
        const char key = static_cast<char>(std::toupper((*it)[1].str()[0]));
        result[key] = std::strtod((*it)[2].str().c_str(), nullptr);
    }
    return result;
}

bool is_layer_marker(const std::string& line)
{
    static const std::regex marker_re(
        R"(^\s*;\s*(?:LAYER_CHANGE\b|CHANGE_LAYER\b|LAYER\s*:\s*-?\d+))",
        std::regex::icase);
    return std::regex_search(line, marker_re);
}

bool parse_z_marker(const std::string& line, double& z)
{
    static const std::regex z_re(
        R"(^\s*;\s*(?:Z_HEIGHT|Z)\s*:\s*(-?\d+(?:\.\d+)?))",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_search(line, match, z_re))
        return false;
    z = std::strtod(match[1].str().c_str(), nullptr);
    return true;
}

std::vector<LayerSpan> layer_spans(const std::vector<std::string>& lines)
{
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (is_layer_marker(lines[i]))
            starts.push_back(i);

    if (starts.empty()) {
        double last_z = std::numeric_limits<double>::quiet_NaN();
        for (std::size_t i = 0; i < lines.size(); ++i) {
            double z = 0.0;
            if (!parse_z_marker(lines[i], z))
                continue;
            if (!std::isfinite(last_z) || std::abs(z - last_z) > 1e-6) {
                starts.push_back(i);
                last_z = z;
            }
        }
    }

    std::vector<LayerSpan> result;
    result.reserve(starts.size());
    for (std::size_t i = 0; i < starts.size(); ++i)
        result.push_back({starts[i], i + 1 < starts.size() ? starts[i + 1] : lines.size()});
    return result;
}

std::string extract_role(const std::string& line, const std::string& current)
{
    const auto semicolon = line.find(';');
    if (semicolon == std::string::npos)
        return current;
    const std::string comment = trim(line.substr(semicolon + 1));
    const std::string lower = lower_copy(comment);
    if (lower.rfind("type:", 0) == 0)
        return trim(comment.substr(5));
    static const std::regex feature_re(R"(^FEATURE\s*:\s*(.*?)\s*$)", std::regex::icase);
    std::smatch match;
    if (std::regex_match(comment, match, feature_re))
        return trim(match[1].str());
    return current;
}

bool role_allowed(const std::string& role, const LayerTimeSmoothConfig& config)
{
    std::string normalized = lower_copy(role);
    std::replace(normalized.begin(), normalized.end(), '_', ' ');
    if (normalized.find("prime tower") != std::string::npos ||
        normalized.find("wipe tower") != std::string::npos ||
        normalized.find("purge") != std::string::npos)
        return false;
    if (normalized.find("bridge") != std::string::npos ||
        normalized.find("overhang") != std::string::npos)
        return config.adjust_bridges;
    if (normalized.find("support") != std::string::npos)
        return config.adjust_support;
    if (normalized.find("gap fill") != std::string::npos ||
        normalized.find("gapfill") != std::string::npos)
        return config.adjust_gap_fill;
    if (normalized.find("top surface") != std::string::npos ||
        normalized.find("top solid") != std::string::npos ||
        normalized.find("ironing") != std::string::npos)
        return config.adjust_top_surface;
    if (normalized.find("outer wall") != std::string::npos ||
        normalized.find("external perimeter") != std::string::npos)
        return !config.preserve_outer_walls;
    if (normalized.find("inner wall") != std::string::npos ||
        normalized.find("internal perimeter") != std::string::npos ||
        normalized.find("perimeter") != std::string::npos)
        return config.adjust_inner_walls;
    if (normalized.find("infill") != std::string::npos ||
        normalized.find("solid infill") != std::string::npos ||
        normalized.find("internal solid") != std::string::npos ||
        normalized.find("sparse infill") != std::string::npos ||
        normalized.find("bottom surface") != std::string::npos)
        return config.adjust_infill;
    return false;
}

Analysis analyze_gcode(const std::vector<std::string>& lines, const LayerTimeSmoothConfig& config)
{
    Analysis result;
    result.spans = layer_spans(lines);
    result.line_to_layer.assign(lines.size(), -1);
    if (result.spans.empty())
        return result;

    result.stats.resize(result.spans.size());
    for (std::size_t li = 0; li < result.spans.size(); ++li)
        for (std::size_t line = result.spans[li].begin; line < result.spans[li].end; ++line)
            result.line_to_layer[line] = static_cast<int>(li);

    double x = std::numeric_limits<double>::quiet_NaN();
    double y = std::numeric_limits<double>::quiet_NaN();
    double z = std::numeric_limits<double>::quiet_NaN();
    double last_e = std::numeric_limits<double>::quiet_NaN();
    bool absolute_xyz = true;
    bool absolute_e = true;
    double units = 1.0;
    double feed = 1800.0;
    std::string role;

    for (std::size_t index = 0; index < lines.size(); ++index) {
        role = extract_role(lines[index], role);
        const std::string code = code_part(lines[index]);
        const std::string cmd = command(code);
        if (cmd.empty())
            continue;
        const auto parsed = words(code);

        if (cmd == "G90") { absolute_xyz = true; continue; }
        if (cmd == "G91") { absolute_xyz = false; continue; }
        if (cmd == "M82") { absolute_e = true; continue; }
        if (cmd == "M83") { absolute_e = false; continue; }
        if (cmd == "G20") { units = 25.4; continue; }
        if (cmd == "G21") { units = 1.0; continue; }
        if (cmd == "G92") {
            if (parsed.count('X')) x = parsed.at('X') * units;
            if (parsed.count('Y')) y = parsed.at('Y') * units;
            if (parsed.count('Z')) z = parsed.at('Z') * units;
            if (parsed.count('E')) last_e = parsed.at('E') * units;
            continue;
        }
        if (cmd != "G0" && cmd != "G00" && cmd != "G1" && cmd != "G01")
            continue;

        if (parsed.count('F'))
            feed = parsed.at('F') * units;

        const auto axis = [&](char key, double old) {
            if (!parsed.count(key))
                return old;
            const double value = parsed.at(key) * units;
            return absolute_xyz || !std::isfinite(old) ? value : old + value;
        };
        const double new_x = axis('X', x);
        const double new_y = axis('Y', y);
        const double new_z = axis('Z', z);
        double distance = 0.0;
        if (std::isfinite(x) && std::isfinite(y) &&
            std::isfinite(new_x) && std::isfinite(new_y)) {
            const double dx = new_x - x;
            const double dy = new_y - y;
            const double dz = std::isfinite(z) && std::isfinite(new_z) ? new_z - z : 0.0;
            distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        x = new_x;
        y = new_y;
        z = new_z;

        bool extrusion = false;
        if (parsed.count('E')) {
            const double extrusion_value = parsed.at('E') * units;
            const double delta = absolute_e ?
                (std::isfinite(last_e) ? extrusion_value - last_e : extrusion_value) :
                extrusion_value;
            if (absolute_e)
                last_e = extrusion_value;
            extrusion = delta > 1e-8;
        }

        const int layer_index = result.line_to_layer[index];
        if (layer_index < 0 || distance <= 1e-8 || feed <= 1e-6)
            continue;

        LayerStats& stats = result.stats[static_cast<std::size_t>(layer_index)];
        const double duration = distance / (feed / 60.0);
        stats.total += duration;
        const bool eligible = extrusion && role_allowed(role, config);
        if (eligible) {
            stats.adjustable += duration;
            ++stats.eligible;
            const double min_speed = config.keep_min_speed ? config.min_speed_mm_s : 1.0;
            const double min_allowed = std::min(feed,
                std::max(min_speed * 60.0,
                    feed * (1.0 - config.max_slowdown_percent / 100.0)));
            const double floor = clampv(min_allowed / feed, 0.01, 1.0);
            stats.eligible_floors.emplace_back(duration, floor);
            stats.max_adjustable_time += duration / floor;
        } else {
            stats.fixed += duration;
        }
    }

    result.times.reserve(result.stats.size());
    for (const LayerStats& stats : result.stats)
        result.times.push_back(std::max(stats.total, 0.001));
    return result;
}

std::vector<double> gaussian_smooth(const std::vector<double>& values, int radius)
{
    if (values.empty() || radius <= 1)
        return values;
    const double sigma = std::max(0.65, static_cast<double>(radius) / 2.0);
    const int half = std::max(1, static_cast<int>(std::ceil(static_cast<double>(radius) * 1.5)));
    std::vector<double> result(values.size(), 0.0);
    for (std::size_t i = 0; i < values.size(); ++i) {
        double total = 0.0;
        double weight_sum = 0.0;
        const int begin = std::max(0, static_cast<int>(i) - half);
        const int end = std::min(static_cast<int>(values.size()) - 1, static_cast<int>(i) + half);
        for (int j = begin; j <= end; ++j) {
            const double delta = static_cast<double>(j) - static_cast<double>(i);
            const double weight = std::exp(-(delta * delta) / (2.0 * sigma * sigma));
            total += values[static_cast<std::size_t>(j)] * weight;
            weight_sum += weight;
        }
        result[i] = total / std::max(weight_sum, 1e-12);
    }
    return result;
}

double profile_at(const std::vector<double>& profile, double position)
{
    if (profile.empty())
        return 1.0;
    const double p = clampv(position, 0.0, 1.0) * static_cast<double>(profile.size() - 1);
    const std::size_t low = static_cast<std::size_t>(std::floor(p));
    const std::size_t high = std::min(profile.size() - 1, low + 1);
    const double fraction = p - static_cast<double>(low);
    return clampv(profile[low] * (1.0 - fraction) + profile[high] * fraction, 0.0, 1.0);
}

std::pair<std::vector<double>, std::vector<double>> project_targets(
    const std::vector<double>& times, const std::vector<LayerStats>& stats, double ratio)
{
    std::vector<double> caps;
    caps.reserve(times.size());
    for (std::size_t i = 0; i < times.size(); ++i) {
        const LayerStats& layer = stats[i];
        if (layer.adjustable <= 1e-9 || layer.eligible == 0)
            caps.push_back(times[i]);
        else
            caps.push_back(layer.fixed + layer.max_adjustable_time);
    }

    std::vector<double> target = times;
    const std::size_t max_passes = std::max<std::size_t>(4, times.size() * 2);
    for (std::size_t pass = 0; pass < max_passes; ++pass) {
        bool changed = false;
        for (std::size_t i = 0; i + 1 < target.size(); ++i) {
            const double need = target[i + 1] / ratio;
            if (target[i] + 1e-7 < need) {
                const double value = std::min(caps[i], need);
                if (value > target[i] + 1e-7) {
                    target[i] = value;
                    changed = true;
                }
            }
        }
        for (std::size_t i = target.size() - 1; i > 0; --i) {
            const double need = target[i - 1] / ratio;
            if (target[i] + 1e-7 < need) {
                const double value = std::min(caps[i], need);
                if (value > target[i] + 1e-7) {
                    target[i] = value;
                    changed = true;
                }
            }
        }
        if (!changed)
            break;
    }
    return {target, caps};
}

std::vector<double> shape_targets(
    const std::vector<double>& times, const std::vector<double>& targets,
    const std::vector<double>& caps, const LayerTimeSmoothConfig& config)
{
    std::vector<double> extra(times.size(), 0.0);
    for (std::size_t i = 0; i < times.size(); ++i)
        extra[i] = std::max(0.0, targets[i] - times[i]);
    const std::vector<double> smooth_extra = gaussian_smooth(extra, config.smooth_radius);
    std::vector<double> shaped;
    shaped.reserve(times.size());
    for (std::size_t i = 0; i < times.size(); ++i) {
        const double strength = profile_at(config.manual_profile,
            static_cast<double>(i) / static_cast<double>(std::max<std::size_t>(1, times.size() - 1)));
        const double value = times[i] + std::max(0.0, smooth_extra[i]) * strength;
        shaped.push_back(clampv(value, times[i], std::max(times[i], caps[i])));
    }
    return shaped;
}

struct ScaleResult {
    std::vector<double> scales;
    std::vector<double> targets;
    double effective_ratio = 1.0;
};

ScaleResult compute_layer_scales(
    const std::vector<double>& times, const std::vector<LayerStats>& stats,
    const LayerTimeSmoothConfig& config)
{
    const double ratio = 1.0 + config.max_layer_variation_percent / 100.0;
    const auto projected = project_targets(times, stats, ratio);
    ScaleResult result;
    result.targets = shape_targets(times, projected.first, projected.second, config);
    result.effective_ratio = ratio;

    const double baseline_total = std::accumulate(times.begin(), times.end(), 0.0);
    const double budget = baseline_total * config.max_total_time_increase_percent / 100.0;
    double extra = 0.0;
    for (std::size_t i = 0; i < times.size(); ++i)
        extra += std::max(0.0, result.targets[i] - times[i]);

    if (extra > budget + 1e-6) {
        double low = ratio;
        double high = std::max(ratio, 10.0);
        std::vector<double> best;
        bool have_best = false;
        for (int iteration = 0; iteration < 36; ++iteration) {
            const double mid = (low + high) / 2.0;
            const auto trial_projected = project_targets(times, stats, mid);
            const std::vector<double> trial = shape_targets(times, trial_projected.first, trial_projected.second, config);
            double increase = 0.0;
            for (std::size_t i = 0; i < times.size(); ++i)
                increase += std::max(0.0, trial[i] - times[i]);
            if (increase <= budget) {
                best = trial;
                high = mid;
                have_best = true;
            } else {
                low = mid;
            }
        }
        if (have_best) {
            result.targets = std::move(best);
            result.effective_ratio = high;
        } else {
            result.targets = times;
            result.effective_ratio = std::numeric_limits<double>::infinity();
        }
    }

    result.scales.reserve(times.size());
    for (std::size_t i = 0; i < times.size(); ++i) {
        const LayerStats& layer = stats[i];
        const double target = result.targets[i];
        if (layer.adjustable <= 1e-9 || layer.eligible == 0 || target <= times[i] + 1e-8) {
            result.scales.push_back(1.0);
            continue;
        }

        const double desired_time = std::max(target - layer.fixed, layer.adjustable);
        double min_floor = 1.0;
        double max_time = 0.0;
        for (const auto& item : layer.eligible_floors) {
            min_floor = std::min(min_floor, item.second);
            max_time += item.first / item.second;
        }
        if (desired_time >= max_time - 1e-8) {
            result.scales.push_back(min_floor);
            continue;
        }

        double low = min_floor;
        double high = 1.0;
        for (int iteration = 0; iteration < 40; ++iteration) {
            const double mid = (low + high) / 2.0;
            double estimate = 0.0;
            for (const auto& item : layer.eligible_floors)
                estimate += item.first / std::max(mid, item.second);
            if (estimate > desired_time)
                low = mid;
            else
                high = mid;
        }
        result.scales.push_back((low + high) / 2.0);
    }
    return result;
}

std::string replace_feed(const std::string& code, double feed, double units)
{
    static const std::regex feed_re(R"(\b[Ff]\s*-?(?:\d+(?:\.\d*)?|\.\d+))");
    const bool trailing_cr = !code.empty() && code.back() == '\r';
    const std::string input = trailing_cr ? code.substr(0, code.size() - 1) : code;
    std::string output = std::regex_replace(input, feed_re, "");
    while (!output.empty() && std::isspace(static_cast<unsigned char>(output.back())) != 0)
        output.pop_back();
    std::ostringstream formatted;
    formatted << output << " F" << std::fixed;
    formatted.precision(1);
    formatted << feed / units;
    if (trailing_cr)
        formatted << '\r';
    return formatted.str();
}

struct RewriteResult {
    std::vector<std::string> lines;
    std::size_t changed_moves = 0;
    std::size_t restored_moves = 0;
};

RewriteResult rewrite_gcode(
    const std::vector<std::string>& lines, const Analysis& analysis,
    const std::vector<double>& scales, const LayerTimeSmoothConfig& config)
{
    RewriteResult result;
    result.lines.reserve(lines.size());
    double x = std::numeric_limits<double>::quiet_NaN();
    double y = std::numeric_limits<double>::quiet_NaN();
    double z = std::numeric_limits<double>::quiet_NaN();
    double last_e = std::numeric_limits<double>::quiet_NaN();
    bool absolute_xyz = true;
    bool absolute_e = true;
    double units = 1.0;
    double original_feed = 1800.0;
    double output_feed = 1800.0;
    std::string role;

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::string& line = lines[index];
        role = extract_role(line, role);
        const std::string code = code_part(line);
        const std::string comment = comment_part(line);
        const std::string cmd = command(code);
        if (cmd.empty()) {
            result.lines.push_back(line);
            continue;
        }
        const auto parsed = words(code);

        if (cmd == "G90") { absolute_xyz = true; result.lines.push_back(line); continue; }
        if (cmd == "G91") { absolute_xyz = false; result.lines.push_back(line); continue; }
        if (cmd == "M82") { absolute_e = true; result.lines.push_back(line); continue; }
        if (cmd == "M83") { absolute_e = false; result.lines.push_back(line); continue; }
        if (cmd == "G20") { units = 25.4; result.lines.push_back(line); continue; }
        if (cmd == "G21") { units = 1.0; result.lines.push_back(line); continue; }
        if (cmd == "G92") {
            if (parsed.count('X')) x = parsed.at('X') * units;
            if (parsed.count('Y')) y = parsed.at('Y') * units;
            if (parsed.count('Z')) z = parsed.at('Z') * units;
            if (parsed.count('E')) last_e = parsed.at('E') * units;
            result.lines.push_back(line);
            continue;
        }
        if (cmd != "G0" && cmd != "G00" && cmd != "G1" && cmd != "G01") {
            result.lines.push_back(line);
            continue;
        }

        if (parsed.count('F'))
            original_feed = parsed.at('F') * units;
        const auto axis = [&](char key, double old) {
            if (!parsed.count(key))
                return old;
            const double value = parsed.at(key) * units;
            return absolute_xyz || !std::isfinite(old) ? value : old + value;
        };
        const double new_x = axis('X', x);
        const double new_y = axis('Y', y);
        const double new_z = axis('Z', z);
        double distance = 0.0;
        if (std::isfinite(x) && std::isfinite(y) &&
            std::isfinite(new_x) && std::isfinite(new_y)) {
            const double dx = new_x - x;
            const double dy = new_y - y;
            const double dz = std::isfinite(z) && std::isfinite(new_z) ? new_z - z : 0.0;
            distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        x = new_x;
        y = new_y;
        z = new_z;

        bool extrusion = false;
        if (parsed.count('E')) {
            const double extrusion_value = parsed.at('E') * units;
            const double delta = absolute_e ?
                (std::isfinite(last_e) ? extrusion_value - last_e : extrusion_value) :
                extrusion_value;
            if (absolute_e)
                last_e = extrusion_value;
            extrusion = delta > 1e-8;
        }

        const int layer_index = analysis.line_to_layer[index];
        const bool eligible = layer_index >= 0 && distance > 1e-8 && extrusion &&
            role_allowed(role, config) && original_feed > 0.0;
        double desired_feed = original_feed;
        if (eligible) {
            const double min_speed = config.keep_min_speed ? config.min_speed_mm_s : 1.0;
            const double min_allowed = std::min(original_feed,
                std::max(min_speed * 60.0,
                    original_feed * (1.0 - config.max_slowdown_percent / 100.0)));
            desired_feed = std::max(original_feed * scales[static_cast<std::size_t>(layer_index)], min_allowed);
        }

        if (eligible && desired_feed < original_feed - 0.05)
            ++result.changed_moves;
        if (!eligible && std::abs(desired_feed - output_feed) > 0.05)
            ++result.restored_moves;

        const bool has_feed_word = parsed.count('F') != 0;
        const bool need_write = std::abs(desired_feed - output_feed) > 0.05 ||
            (has_feed_word && std::abs(desired_feed - original_feed) > 0.05);
        if (need_write)
            result.lines.push_back(replace_feed(code, desired_feed, units) + comment);
        else
            result.lines.push_back(line);
        output_feed = desired_feed;
    }
    return result;
}

double max_relative_jump(const std::vector<double>& values)
{
    double result = 0.0;
    for (std::size_t i = 1; i < values.size(); ++i)
        result = std::max(result, std::abs(values[i] - values[i - 1]) /
            std::max(1e-6, std::min(values[i], values[i - 1])));
    return result;
}

double sum_values(const std::vector<double>& values)
{
    return std::accumulate(values.begin(), values.end(), 0.0);
}

std::string join_lines(const std::vector<std::string>& lines, bool trailing_newline)
{
    std::string result;
    std::size_t size = trailing_newline ? 1 : 0;
    for (const std::string& line : lines)
        size += line.size() + 1;
    result.reserve(size);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        result += lines[i];
        if (i + 1 < lines.size() || trailing_newline)
            result += '\n';
    }
    return result;
}
} // namespace

bool LayerTimeSmooth::process(
    std::string& gcode, const LayerTimeSmoothConfig& input, LayerTimeSmoothReport* report)
{
    LayerTimeSmoothReport result;
    const auto finish = [&](LayerTimeSmoothReport::Status status, const std::string& message, bool changed = false) {
        result.status = status;
        result.message = message;
        if (report != nullptr)
            *report = result;
        return changed;
    };

    if (!input.enabled)
        return finish(LayerTimeSmoothReport::Status::Disabled, "Layer Time Smooth disabled.");
    if (gcode.find("LAYER_TIME_SMOOTH_V5_PROCESSED") != std::string::npos ||
        gcode.find("LAYER_TIME_SMOOTH_V521_NATIVE") != std::string::npos)
        return finish(LayerTimeSmoothReport::Status::AlreadyProcessed, "G-code already processed; skipped.");

    LayerTimeSmoothConfig config = input;
    config.max_slowdown_percent = clampv(config.max_slowdown_percent, 0.0, 70.0);
    config.min_speed_mm_s = clampv(config.min_speed_mm_s, 1.0, 100.0);
    config.max_total_time_increase_percent = clampv(config.max_total_time_increase_percent, 0.0, 50.0);
    config.quality_speed = clampv(config.quality_speed, 0.0, 1.0);
    config.max_layer_variation_percent = 10.0 + 40.0 * config.quality_speed;
    config.smooth_radius = static_cast<int>(clampv(config.smooth_radius, 1.0, 20.0));
    if (config.manual_profile.size() != 64)
        config.manual_profile.assign(64, 1.0);
    for (double& value : config.manual_profile)
        value = clampv(value, 0.0, 1.0);

    const bool trailing_newline = !gcode.empty() && gcode.back() == '\n';
    std::vector<std::string> lines;
    std::istringstream input_stream(gcode);
    std::string line;
    while (std::getline(input_stream, line))
        lines.push_back(line);

    const Analysis before = analyze_gcode(lines, config);
    if (before.spans.size() < 3)
        return finish(LayerTimeSmoothReport::Status::NoLayers,
            "Fewer than three reliable layer markers; G-code unchanged.");

    const ScaleResult scales = compute_layer_scales(before.times, before.stats, config);
    RewriteResult rewritten = rewrite_gcode(lines, before, scales.scales, config);
    if (rewritten.changed_moves == 0)
        return finish(LayerTimeSmoothReport::Status::NoChange,
            "No eligible extrusion moves could be safely slowed.");

    const Analysis after = analyze_gcode(rewritten.lines, config);
    const double total_before = sum_values(before.times);
    const double total_after = sum_values(after.times);
    result.layers = before.spans.size();
    result.changed_moves = rewritten.changed_moves;
    result.restored_moves = rewritten.restored_moves;
    result.time_before = total_before;
    result.time_after = total_after;
    result.max_jump_before = max_relative_jump(before.times);
    result.max_jump_after = max_relative_jump(after.times);
    result.effective_layer_ratio = scales.effective_ratio;
    result.scale_profile = scales.scales;

    const bool crlf = !lines.empty() && !lines[before.spans.front().begin].empty() &&
        lines[before.spans.front().begin].back() == '\r';
    const std::string line_suffix = crlf ? "\r" : "";
    std::vector<std::string> markers;
    markers.push_back("; LAYER_TIME_SMOOTH_V5_PROCESSED=1" + line_suffix);
    if (config.write_layer_report) {
        std::ostringstream estimate;
        estimate << "; LTS_V5_ESTIMATED_TIME_BEFORE=" << std::fixed << std::setprecision(2)
                 << total_before << "s AFTER=" << total_after << "s" << line_suffix;
        markers.push_back(estimate.str());
    }
    std::ostringstream details;
    details << "; LTS_V5_LAYERS=" << before.spans.size()
            << " CHANGED_EXTRUSION_MOVES=" << rewritten.changed_moves
            << " RESTORE_FEED_MOVES=" << rewritten.restored_moves << line_suffix;
    markers.push_back(details.str());
    rewritten.lines.insert(rewritten.lines.begin() + static_cast<std::ptrdiff_t>(before.spans.front().begin),
        markers.begin(), markers.end());

    std::ostringstream message;
    message << "V5.2.1 processed " << before.spans.size() << " layers; adjusted "
            << rewritten.changed_moves << " extrusion moves and restored "
            << rewritten.restored_moves << " non-target moves. Estimated time "
            << std::fixed << std::setprecision(1) << total_before << "s -> " << total_after
            << "s; maximum adjacent-layer variation "
            << result.max_jump_before * 100.0 << "% -> " << result.max_jump_after * 100.0 << "%.";
    const double requested_ratio = 1.0 + config.max_layer_variation_percent / 100.0;
    if (std::isfinite(scales.effective_ratio) &&
        scales.effective_ratio > requested_ratio + 1e-3) {
        message << " Total-time budget relaxed the target to "
                << (scales.effective_ratio - 1.0) * 100.0 << "%.";
    }
    if (result.max_jump_after > config.max_layer_variation_percent / 100.0 + 0.005)
        message << " Some layers remain above target due to minimum-speed or slowdown limits.";
    result.status = LayerTimeSmoothReport::Status::Processed;
    result.message = message.str();

    gcode = join_lines(rewritten.lines, trailing_newline);
    if (report != nullptr)
        *report = result;
    return true;
}
} // namespace Slic3r::GCodeSmoothing
