#include "../src/libslic3r/Connector/PaintedSurfaceConnector.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace Slic3r::Connector;

static bool close(double a,double b) { return std::abs(a-b)<1e-8; }
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
    bool bottom=false;
    for(auto const& f:result.triangles)
        if(close(f.a.z,0)&&close(f.b.z,0)&&close(f.c.z,0))bottom=true;
    assert(bottom);

    std::vector<Triangle> separate=patch;
    separate.push_back({{20,0,6},{30,0,6},{20,10,6}});
    assert(connected_painted_regions(separate).size()==2);
    assert(generate_downward_connectors(separate).downward_connectors==2);
    assert(generate_downward_connectors({}).triangles.empty());
    std::cout<<"Painted Surface Connector native geometry tests passed\n";
}
