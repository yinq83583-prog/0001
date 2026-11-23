#include "../src/libslic3r/Connector/PaintedSurfaceConnector.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <tuple>
#include <algorithm>
using namespace Slic3r::Connector;

static bool close(double a,double b) { return std::abs(a-b)<1e-8; }
static bool is_closed_manifold(const std::vector<Triangle>& triangles) {
    using Vertex = std::tuple<long long,long long,long long>;
    using Edge = std::pair<Vertex,Vertex>;
    struct EdgeUse { int count = 0; int orientation = 0; };
    auto key=[](Vec3 p) -> Vertex {
        return {std::llround(p.x*100000),std::llround(p.y*100000),std::llround(p.z*100000)};
    };
    std::map<Edge,EdgeUse> uses;
    double signed_volume6 = 0.0;
    for(const Triangle& t:triangles) {
        Vertex a=key(t.a),b=key(t.b),c=key(t.c);
        for(const auto& directed:{std::pair<Vertex,Vertex>{a,b},{b,c},{c,a}}) {
            if(directed.first==directed.second)return false;
            Edge e=directed.first<directed.second?
                Edge{directed.first,directed.second}:Edge{directed.second,directed.first};
            auto& use=uses[e];
            ++use.count;
            use.orientation += directed.first<directed.second?1:-1;
        }
        const Vec3 bc={t.b.y*t.c.z-t.b.z*t.c.y,
                       t.b.z*t.c.x-t.b.x*t.c.z,
                       t.b.x*t.c.y-t.b.y*t.c.x};
        signed_volume6 += t.a.x*bc.x+t.a.y*bc.y+t.a.z*bc.z;
    }
    return !uses.empty()&&std::abs(signed_volume6)>1e-9&&
        std::all_of(uses.begin(),uses.end(),[](const auto& item) {
            return item.second.count==2&&item.second.orientation==0;
        });
}
int main() {
    std::vector<Triangle> patch={
        {{0,0,5},{10,0,5},{10,10,5}},
        {{0,0,5},{10,10,5},{0,10,5}}
    };
    auto regions=connected_painted_regions(patch);
    assert(regions.size()==1);
    assert(regions.front().facets.size()==2);
    assert(close(regions.front().area,100.0));
    assert(close(regions.front().center.z,5.0));
    assert(close(regions.front().normal.z,1.0));
    auto result=generate_downward_connectors(patch);
    assert(result.regions==1);
    assert(result.downward_connectors==1);
    assert(result.skipped_regions==0);
    assert(result.triangles.size()==12); // 2 top + 2 bottom + 8 wall
    assert(is_closed_manifold(result.triangles));
    bool bottom=false;
    for(auto const& f:result.triangles)
        if(close(f.a.z,0)&&close(f.b.z,0)&&close(f.c.z,0))bottom=true;
    assert(bottom);

    // A positive bed flare widens the footprint without opening the shell.
    ConnectorOptions no_flare;
    no_flare.bed_flare_mm = 0.0;
    const auto straight = generate_downward_connectors(patch, no_flare);
    ConnectorOptions with_flare;
    with_flare.bed_flare_mm = 1.2;
    const auto widened = generate_downward_connectors(patch, with_flare);
    assert(straight.downward_connectors == 1);
    assert(widened.downward_connectors == 1);
    assert(is_closed_manifold(widened.triangles));
    double straight_min_x = 1e9, widened_min_x = 1e9;
    for (const Triangle& t : straight.triangles)
        for (const Vec3 v : {t.a, t.b, t.c})
            if (close(v.z, 0.0)) straight_min_x = std::min(straight_min_x, v.x);
    for (const Triangle& t : widened.triangles)
        for (const Vec3 v : {t.a, t.b, t.c})
            if (close(v.z, 0.0)) widened_min_x = std::min(widened_min_x, v.x);
    assert(widened_min_x < straight_min_x - 0.5);

    std::vector<Triangle> separate=patch;
    separate.push_back({{20,0,6},{30,0,6},{20,10,6}});
    assert(connected_painted_regions(separate).size()==2);
    assert(generate_downward_connectors(separate).downward_connectors==2);
    assert(generate_downward_connectors({}).triangles.empty());
    const std::vector<Triangle> facing={
        {{0,0,3},{10,0,3},{0,10,3}},
        {{0,0,8},{0,10,8},{10,0,8}}
    };
    auto connected=generate_connectors(facing);
    assert(connected.regions==2);
    assert(connected.bridges==1);
    assert(connected.downward_connectors==0);
    assert(connected.triangles.size()==8); // 6 side + 2 conformal caps
    assert(is_closed_manifold(connected.triangles));

    // Different boundary triangulations are resampled to a common loop.
    const std::vector<Triangle> mismatched_loops={
        {{0,0,3},{10,0,3},{10,10,3}},
        {{0,0,3},{10,10,3},{0,10,3}},
        {{0,0,8},{0,10,8},{5,0,8}},
        {{5,0,8},{10,10,8},{10,0,8}},
        {{5,0,8},{0,10,8},{10,10,8}}
    };
    auto resampled_bridge=generate_connectors(mismatched_loops);
    assert(resampled_bridge.regions==2);
    assert(resampled_bridge.bridges==1);
    assert(resampled_bridge.downward_connectors==0);
    assert(resampled_bridge.triangles.size()==14); // 9 zipper sides + 5 original cap faces
    assert(is_closed_manifold(resampled_bridge.triangles));

    // Distant painted islands are not accidentally joined.
    const std::vector<Triangle> distant={
        {{0,0,3},{10,0,3},{0,10,3}},
        {{0,0,40},{0,10,40},{10,0,40}}
    };
    auto distant_result=generate_connectors(distant);
    assert(distant_result.bridges==0);
    assert(distant_result.downward_connectors==2);

    // Same-facing patches must never create a bridge.
    const std::vector<Triangle> same_facing={
        {{0,0,3},{10,0,3},{0,10,3}},
        {{0,0,8},{10,0,8},{0,10,8}}
    };
    auto downward=generate_connectors(same_facing);
    assert(downward.regions==2);
    assert(downward.bridges==0);
    assert(downward.downward_connectors==2);

    // A painted patch almost on the bed is not turned into a zero-height column.
    const std::vector<Triangle> near_bed={
        {{0,0,0.1},{10,0,0.1},{0,10,0.1}}
    };
    auto skipped=generate_downward_connectors(near_bed);
    assert(skipped.regions==1);
    assert(skipped.downward_connectors==0);
    assert(skipped.skipped_regions==1);
    assert(skipped.triangles.empty());
    // A vertical painted patch projects to a zero-area footprint on the bed.
    // It must be skipped rather than emitted as an open, zero-volume shell.
    const std::vector<Triangle> vertical_patch={
        {{0,0,5},{10,0,5},{10,0,15}},
        {{0,0,5},{10,0,15},{0,0,15}}
    };
    auto vertical_result=generate_downward_connectors(vertical_patch);
    assert(vertical_result.regions==1);
    assert(vertical_result.downward_connectors==0);
    assert(vertical_result.skipped_regions==1);
    assert(vertical_result.triangles.empty());

    // A sloped patch with a non-degenerate bed footprint remains a valid solid.
    const std::vector<Triangle> sloped_patch={
        {{0,0,5},{10,0,7},{0,10,6}}
    };
    auto sloped_result=generate_downward_connectors(sloped_patch);
    assert(sloped_result.downward_connectors==1);
    assert(sloped_result.skipped_regions==0);
    assert(is_closed_manifold(sloped_result.triangles));

    // A patch that intersects the build plate is rejected as a support source.
    const std::vector<Triangle> bed_intersection={
        {{0,0,-1},{10,0,5},{0,10,5}}
    };
    auto bed_intersection_result=generate_downward_connectors(bed_intersection);
    assert(bed_intersection_result.downward_connectors==0);
    assert(bed_intersection_result.skipped_regions==1);
    assert(bed_intersection_result.triangles.empty());

    // Curved (non-coplanar) mask: local vertex normals must vary across
    // the patch while the extracted cap and bed extrusion remain watertight.
    const std::vector<Triangle> curved_patch={
        {{0,0,5},{10,0,5},{10,10,6}},
        {{0,0,5},{10,10,6},{0,10,5}}
    };
    ConnectorOptions curved_options;
    curved_options.fit_gap_mm=0.5;
    auto curved=generate_downward_connectors(curved_patch,curved_options);
    assert(curved.downward_connectors==1);
    assert(curved.skipped_regions==0);
    assert(is_closed_manifold(curved.triangles));
    const auto curved_regions=connected_painted_regions(curved_patch);
    const Vec3 average=curved_regions.front().normal;
    bool found_local_normal=false;
    for(const Triangle& tri:curved.triangles)
        for(const Vec3 v:{tri.a,tri.b,tri.c}) {
            // The (10,0,5) corner belongs to just the first sloped face;
            // its offset must not be the patch's averaged normal.
            if(std::abs(v.x-10.0)<1.0 && std::abs(v.y)<1.0 && v.z>4.0 &&
               std::abs((v.y-0.0)-average.y*0.5)>1e-3)
                found_local_normal=true;
        }
    assert(found_local_normal);

    std::cout<<"Painted Surface Connector native geometry tests passed\n";
}
