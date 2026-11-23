#pragma once

#include "PaintedSurfaceConnector.hpp"

namespace Slic3r::Connector {

// Similar to a mask-to-extract workflow: create an independent, printable
// shell conforming to the source triangles. This does NOT alter connector
// annotations, support painting, or the source model.
struct SurfaceExtractOptions {
    double thickness_mm = 1.2;
    double fit_gap_mm = 0.15;
    double boundary_smooth = 0.0; // 0 = exact painted boundary; 1 = gentle boundary relaxation
    bool double_sided = false;    // false = outward shell; true = thickness straddles the surface
};

struct SurfaceExtractResult {
    std::vector<Triangle> triangles;
    std::size_t regions = 0;
    std::size_t extracted_regions = 0;
    std::size_t skipped_regions = 0;
};

SurfaceExtractResult extract_painted_surface(
    const std::vector<Triangle>& painted_facets,
    const SurfaceExtractOptions& options = {});

} // namespace Slic3r::Connector
