#pragma once
#include <array>
#include <cstddef>
#include <vector>

namespace Slic3r::Connector {
struct Vec3 {
    double x=0, y=0, z=0;
};
struct Triangle {
    Vec3 a,b,c;
};
struct Region {
    std::vector<Triangle> facets;
    Vec3 center;
    Vec3 normal;
    double area=0.0;
};
struct ConnectorOptions {
    double fit_gap_mm=0.15;
    double bed_z_mm=0.0;
    double min_height_mm=0.25;
    // Future native bridge generation uses distinct facing regions.
};
struct ConnectorResult {
    std::vector<Triangle> triangles;
    std::size_t regions=0;
    std::size_t bridges=0;
    std::size_t downward_connectors=0;
    std::size_t skipped_regions=0;
};
// Geometry is generated from a painted triangle patch already in the model's
// world/build-plate coordinates. No dependency on Python or exported 3MF.
std::vector<Region> connected_painted_regions(const std::vector<Triangle>& painted_facets);
// Pair distinct mutually facing regions with compatible single-boundary loops;
 // all other regions use the downward connector fallback.
ConnectorResult generate_connectors(
    const std::vector<Triangle>& painted_facets, const ConnectorOptions& options = {});
ConnectorResult generate_downward_connectors(
    const std::vector<Triangle>& painted_facets, const ConnectorOptions& options = {});
} // namespace Slic3r::Connector
