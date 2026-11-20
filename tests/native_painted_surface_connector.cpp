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
    auto key=[](Vec3 p) -> Vertex {
        return {std::llround(p.x*100000),std::llround(p.y*100000),std::llround(p.z*100000)};
    };
    std::map<Edge,int> counts;
    for(const Triangle& t:triangles) {
        Vertex a=key(t.a),b=key(t.b),c=key(t.c);
        for(Edge e:{Edge{a,b},Edge{b,c},Edge{c,a}}) {
            if(e.second<e.first)std::swap(e.first,e.second);
            ++counts[e];
        }
    }
    return !counts.empty()&&std::all_of(counts.begin(),counts.end(),
        [](const auto& edge_count){return edge_count.second==2;});
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
    std::cout<<"Painted Surface Connector native geometry tests passed\n";
}
