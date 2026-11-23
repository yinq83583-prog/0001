#include "SurfaceMaskExtract.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace Slic3r::Connector {
namespace {
Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 mul(Vec3 v, double s) { return {v.x * s, v.y * s, v.z * s}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
double dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
double norm(Vec3 v) { return std::sqrt(dot(v,v)); }
Vec3 unit(Vec3 v) { const double n=norm(v); return n>1e-12?mul(v,1/n):Vec3{}; }
bool finite(Vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }

using Key=std::tuple<std::int64_t,std::int64_t,std::int64_t>;
using Edge=std::pair<Key,Key>;
Key key(Vec3 p) {
    return {std::llround(p.x*100000), std::llround(p.y*100000), std::llround(p.z*100000)};
}
Edge edge(Key a, Key b) { return a<b?Edge{a,b}:Edge{b,a}; }
struct EdgeUse {
    std::size_t count=0;
    int orientation=0;
    Key from,to;
};

// Validate the generated shell before it becomes a printable ModelVolume.
// Each edge must occur exactly twice with opposite orientation.
bool closed_oriented_solid(const std::vector<Triangle>& triangles) {
    if(triangles.size()<4)return false;
    std::map<Edge,EdgeUse> edges;
    double volume6=0;
    for(const Triangle& t:triangles) {
        if(!finite(t.a)||!finite(t.b)||!finite(t.c)||
           norm(cross(sub(t.b,t.a),sub(t.c,t.a)))<1e-10)return false;
        const Key a=key(t.a),b=key(t.b),c=key(t.c);
        for(const auto& e:{std::pair<Key,Key>{a,b},{b,c},{c,a}}) {
            if(e.first==e.second)return false;
            auto& use=edges[edge(e.first,e.second)];
            ++use.count;
            use.orientation+=e.first<e.second?1:-1;
        }
        volume6+=dot(t.a,cross(t.b,t.c));
    }
    for(const auto& e:edges)
        if(e.second.count!=2||e.second.orientation!=0)return false;
    return std::isfinite(volume6)&&std::abs(volume6)>1e-9;
}

bool build_shell(const Region& region,const SurfaceExtractOptions& options,
                 std::vector<Triangle>& result) {
    if(region.facets.empty())return false;
    std::map<Key,Vec3> positions,normals;
    std::map<Edge,EdgeUse> edges;
    for(const Triangle& t:region.facets) {
        if(!finite(t.a)||!finite(t.b)||!finite(t.c))return false;
        const Vec3 n=cross(sub(t.b,t.a),sub(t.c,t.a));
        if(norm(n)<1e-10)return false;
        const Key a=key(t.a),b=key(t.b),c=key(t.c);
        if(a==b||b==c||c==a)return false;
        for(const auto& p:{std::pair<Key,Vec3>{a,t.a},{b,t.b},{c,t.c}}) {
            positions.emplace(p.first,p.second);
            normals[p.first]=add(normals[p.first],n);
        }
        for(const auto& e:{std::pair<Key,Key>{a,b},{b,c},{c,a}}) {
            auto& use=edges[edge(e.first,e.second)];
            ++use.count;
            use.orientation+=e.first<e.second?1:-1;
            use.from=e.first;
            use.to=e.second;
            if(use.count>2)return false;
        }
    }
    for(auto& item:normals) {
        item.second=unit(item.second);
        if(norm(item.second)<1e-8)return false;
    }
    std::map<Key,Key> prev,next;
    std::vector<std::pair<Key,Key>> boundary;
    for(const auto& item:edges) {
        const EdgeUse& use=item.second;
        if(use.count==2) {
            if(use.orientation!=0)return false;
            continue;
        }
        if(use.count!=1||prev.count(use.to)||next.count(use.from))return false;
        prev.emplace(use.to,use.from);
        next.emplace(use.from,use.to);
        boundary.emplace_back(use.from,use.to);
    }
    // Allows separate loops and holes, but not branching or dangling outlines.
    if(prev.size()!=next.size())return false;
    for(const auto& item:next)if(!prev.count(item.first))return false;

    // Surface-tangent boundary relaxation. Keep every cap vertex shared by
    // its triangles. If relaxation flips a cap face, use the original mask.
    std::map<Key,Vec3> relaxed=positions;
    const double smoothing=std::clamp(options.boundary_smooth,0.0,1.0);
    if(smoothing>0&&!boundary.empty()) {
        for(int iter=0;iter<2;++iter) {
            auto next_positions=relaxed;
            for(const auto& item:next) {
                const Key& k=item.first;
                const Vec3 p=relaxed.at(k);
                const Vec3 left=relaxed.at(prev.at(k)),right=relaxed.at(item.second);
                Vec3 delta=sub(mul(add(left,right),0.5),p);
                const Vec3 normal=normals.at(k);
                delta=sub(delta,mul(normal,dot(delta,normal)));
                delta=mul(delta,0.2*smoothing);
                const double max_move=0.24*std::min(norm(sub(left,p)),norm(sub(right,p)));
                if(norm(delta)>max_move&&max_move>0)delta=mul(unit(delta),max_move);
                next_positions[k]=add(p,delta);
            }
            relaxed.swap(next_positions);
        }
        for(const Triangle& t:region.facets) {
            const Vec3 n=cross(sub(t.b,t.a),sub(t.c,t.a));
            const Vec3 a=relaxed.at(key(t.a)),b=relaxed.at(key(t.b)),c=relaxed.at(key(t.c));
            const Vec3 n2=cross(sub(b,a),sub(c,a));
            if(dot(n,n2)<0.15*dot(n,n)) {
                relaxed=positions;
                break;
            }
        }
    }

    const double top_offset=options.fit_gap_mm+
        (options.double_sided?0.5*options.thickness_mm:options.thickness_mm);
    const double bottom_offset=options.fit_gap_mm-
        (options.double_sided?0.5*options.thickness_mm:0.0);
    std::map<Key,Vec3> top,bottom;
    for(const auto& item:relaxed) {
        const Vec3 n=normals.at(item.first);
        top.emplace(item.first,add(item.second,mul(n,top_offset)));
        bottom.emplace(item.first,add(item.second,mul(n,bottom_offset)));
    }
    std::vector<Triangle> candidate;
    candidate.reserve(2*(region.facets.size()+boundary.size()));
    for(const Triangle& t:region.facets) {
        const Key a=key(t.a),b=key(t.b),c=key(t.c);
        candidate.push_back({top.at(a),top.at(b),top.at(c)});
        candidate.push_back({bottom.at(a),bottom.at(c),bottom.at(b)});
    }
    for(const auto& e:boundary) {
        const Vec3 ta=top.at(e.first),tb=top.at(e.second);
        const Vec3 ba=bottom.at(e.first),bb=bottom.at(e.second);
        candidate.push_back({tb,ta,ba});
        candidate.push_back({tb,ba,bb});
    }
    // Don't generate printable material beneath the build plate.
    for(const Triangle& t:candidate)
        if(t.a.z < -1e-5 || t.b.z < -1e-5 || t.c.z < -1e-5)
            return false;
    if(!closed_oriented_solid(candidate))return false;
    result.insert(result.end(),candidate.begin(),candidate.end());
    return true;
}
} // namespace

SurfaceExtractResult extract_painted_surface(
    const std::vector<Triangle>& painted_facets,const SurfaceExtractOptions& options) {
    SurfaceExtractResult result;
    const std::vector<Region> regions=connected_painted_regions(painted_facets);
    result.regions=regions.size();
    if(!std::isfinite(options.thickness_mm)||!std::isfinite(options.fit_gap_mm)||
       !std::isfinite(options.boundary_smooth)||options.thickness_mm<0.01||
       options.thickness_mm>100.0||options.fit_gap_mm<0.0||options.fit_gap_mm>20.0) {
        result.skipped_regions=result.regions;
        return result;
    }
    for(const Region& region:regions) {
        if(build_shell(region,options,result.triangles))++result.extracted_regions;
        else ++result.skipped_regions;
    }
    return result;
}
} // namespace Slic3r::Connector
