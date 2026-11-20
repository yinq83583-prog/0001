#include "PaintedSurfaceConnector.hpp"
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <map>
#include <limits>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace Slic3r::Connector {
namespace {
Vec3 sub(Vec3 a,Vec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
Vec3 add(Vec3 a,Vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 mul(Vec3 a,double s){return {a.x*s,a.y*s,a.z*s};}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
double dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
double len(Vec3 a){return std::sqrt(dot(a,a));}
Vec3 unit(Vec3 a){double l=len(a);return l>1e-12?mul(a,1/l):Vec3{};}
using Key=std::tuple<std::int64_t,std::int64_t,std::int64_t>;
Key key(Vec3 v){return {std::llround(v.x*100000),std::llround(v.y*100000),std::llround(v.z*100000)};}
using Edge=std::pair<Key,Key>;
Edge edge(Vec3 a,Vec3 b){Key ka=key(a),kb=key(b);return ka<kb?Edge{ka,kb}:Edge{kb,ka};}
class DisjointSet {
    std::vector<std::size_t> parent;
public:
    explicit DisjointSet(std::size_t n):parent(n){for(std::size_t i=0;i<n;++i)parent[i]=i;}
    std::size_t find(std::size_t i){while(parent[i]!=i){parent[i]=parent[parent[i]];i=parent[i];}return i;}
    void join(std::size_t a,std::size_t b){a=find(a);b=find(b);if(a!=b)parent[b]=a;}
};
void add_triangle_if_valid(std::vector<Triangle>& tris,Vec3 a,Vec3 b,Vec3 c){
    if(len(cross(sub(b,a),sub(c,a)))>1e-12)
        tris.push_back({a,b,c});
}
void add_quad(std::vector<Triangle>& tris,Vec3 a,Vec3 b,Vec3 c,Vec3 d){
    add_triangle_if_valid(tris,a,b,c);
    add_triangle_if_valid(tris,a,c,d);
}
// Reject zero-volume, open, or inconsistently wound connector shells before
// they can be inserted into the printable model.
bool is_closed_oriented_volume(const std::vector<Triangle>& triangles) {
    if (triangles.size() < 4)
        return false;
    struct EdgeUse {
        std::size_t count = 0;
        int orientation = 0;
    };
    std::map<Edge, EdgeUse> uses;
    double signed_volume6 = 0.0;
    for (const Triangle& triangle : triangles) {
        if (len(cross(sub(triangle.b, triangle.a), sub(triangle.c, triangle.a))) <= 1e-12)
            return false;
        for (const auto& directed : {
            std::pair<Vec3, Vec3>{triangle.a, triangle.b},
            {triangle.b, triangle.c},
            {triangle.c, triangle.a}
        }) {
            const Key ka = key(directed.first);
            const Key kb = key(directed.second);
            if (ka == kb)
                return false;
            const Edge e = ka < kb ? Edge{ka, kb} : Edge{kb, ka};
            auto& use = uses[e];
            ++use.count;
            use.orientation += ka < kb ? 1 : -1;
        }
        signed_volume6 += dot(triangle.a, cross(triangle.b, triangle.c));
    }
    for (const auto& item : uses)
        if (item.second.count != 2 || item.second.orientation != 0)
            return false;
    return std::abs(signed_volume6) > 1e-9;
}
}
std::vector<Region> connected_painted_regions(const std::vector<Triangle>& painted) {
    if(painted.empty())return {};
    DisjointSet ds(painted.size());
    std::map<Edge,std::size_t> owners;
    for(std::size_t i=0;i<painted.size();++i){
        const auto& t=painted[i];
        for(const Edge& e:{edge(t.a,t.b),edge(t.b,t.c),edge(t.c,t.a)}){
            auto [it,inserted]=owners.emplace(e,i);
            if(!inserted)ds.join(i,it->second);
        }
    }
    std::map<std::size_t,Region> grouped;
    for(std::size_t i=0;i<painted.size();++i){
        const Triangle& t=painted[i];
        Vec3 n=cross(sub(t.b,t.a),sub(t.c,t.a));
        double area=len(n)*.5;
        if(area<=1e-12)continue;
        Region& region=grouped[ds.find(i)];
        region.facets.push_back(t);
        region.center=add(region.center,mul(add(add(t.a,t.b),t.c),area/3.0));
        region.normal=add(region.normal,n);
        region.area+=area;
    }
    std::vector<Region> result;
    for(auto& kv:grouped){
        Region& r=kv.second;
        if(r.area>0){r.center=mul(r.center,1/r.area);r.normal=unit(r.normal);result.push_back(std::move(r));}
    }
    return result;
}
ConnectorResult generate_downward_connectors(const std::vector<Triangle>& painted,
    const ConnectorOptions& options) {
    ConnectorResult result;
    const auto regions=connected_painted_regions(painted);
    result.regions=regions.size();
    const double gap=std::max(0.0,options.fit_gap_mm);
    for(const Region& r:regions){
        double min_z = std::numeric_limits<double>::infinity();
        for (const Triangle& t : r.facets)
            min_z = std::min({min_z, t.a.z, t.b.z, t.c.z});
        if(min_z<=options.bed_z_mm+std::max(0.0,options.min_height_mm)){
            ++result.skipped_regions;continue;
        }
        std::map<Edge,std::pair<Vec3,Vec3>> borders;
        std::set<Edge> duplicate;
        for(const Triangle& t:r.facets){
            for(const auto& [a,b]:{std::pair<Vec3,Vec3>{t.a,t.b},{t.b,t.c},{t.c,t.a}}){
                const Edge e=edge(a,b);
                if(borders.count(e))duplicate.insert(e);
                else borders.emplace(e,std::make_pair(a,b));
            }
        }
        const Vec3 offset=mul(r.normal,gap);
        std::vector<Triangle> candidate;
        // Conformal cap: preserve the actual painted triangles and their winding.
        for(const Triangle& t:r.facets){
            const Triangle top{add(t.a,offset),add(t.b,offset),add(t.c,offset)};
            add_triangle_if_valid(candidate,top.a,top.b,top.c);
            Triangle bottom=top;
            bottom.a.z=options.bed_z_mm;bottom.b.z=options.bed_z_mm;bottom.c.z=options.bed_z_mm;
            std::swap(bottom.b,bottom.c);
            if(len(cross(sub(bottom.b,bottom.a),sub(bottom.c,bottom.a)))>1e-12)
                candidate.push_back(bottom);
        }
        // Only boundary edges receive walls; shared triangle edges are internal.
        for(const auto& item:borders){
            if(duplicate.count(item.first))continue;
            Vec3 a=add(item.second.first,offset),b=add(item.second.second,offset);
            Vec3 c=b,d=a;c.z=options.bed_z_mm;d.z=options.bed_z_mm;
            add_quad(candidate,b,a,d,c);
        }
        if (!is_closed_oriented_volume(candidate)) {
            ++result.skipped_regions;
            continue;
        }
        result.triangles.insert(result.triangles.end(), candidate.begin(), candidate.end());
        ++result.downward_connectors;
    }
    return result;
}

namespace {
using BoundarySegments=std::map<Edge,std::pair<Vec3,Vec3>>;
bool get_boundary(const Region& region,std::vector<Vec3>& loop) {
    BoundarySegments edges;
    std::set<Edge> interior;
    for(const auto& t:region.facets)
        for(const auto& [a,b]:{std::pair<Vec3,Vec3>{t.a,t.b},{t.b,t.c},{t.c,t.a}}) {
            Edge e=edge(a,b);
            if(edges.count(e))interior.insert(e);
            else edges.emplace(e,std::make_pair(a,b));
        }
    std::map<Key,Vec3> from,to,vertex_positions;
    for(const auto& e:edges){
        if(interior.count(e.first))continue;
        const Key ka=key(e.second.first),kb=key(e.second.second);
        if(from.count(ka)||to.count(kb))return false;
        from.emplace(ka,e.second.second);
        to.emplace(kb,e.second.first);
        vertex_positions.emplace(ka,e.second.first);
        vertex_positions.emplace(kb,e.second.second);
    }
    if(from.size()<3)return false;
    const Key start=from.begin()->first;
    Key curr=start;
    for(std::size_t i=0;i<from.size();++i) {
        if(!from.count(curr)||!vertex_positions.count(curr))return false;
        loop.push_back(vertex_positions.at(curr));
        curr=key(from.at(curr));
        if(curr==start)return loop.size()==from.size();
    }
    return false;
}
void append_cap(std::vector<Triangle>& dst,const Region& region,Vec3 offset) {
    // The painted source normals point into the gap, hence reverse
    // them when using those patches as the outer caps of the connector.
    for(const Triangle& tri:region.facets)
        dst.push_back({add(tri.a,offset),add(tri.c,offset),add(tri.b,offset)});
}
std::vector<double> normalized_loop_fractions(const std::vector<Vec3>& loop) {
    std::vector<double> fractions(loop.size()+1,0.0);
    for(std::size_t i=0;i<loop.size();++i)
        fractions[i+1]=fractions[i]+len(sub(loop[(i+1)%loop.size()],loop[i]));
    const double perimeter=fractions.back();
    if(perimeter>1e-9)
        for(double& value:fractions)value/=perimeter;
    return fractions;
}
std::vector<Vec3> resample_loop(const std::vector<Vec3>& loop,std::size_t count) {
    if(loop.size()<3||count<3)return {};
    std::vector<double> cumulative(loop.size()+1,0.0);
    for(std::size_t i=0;i<loop.size();++i)
        cumulative[i+1]=cumulative[i]+len(sub(loop[(i+1)%loop.size()],loop[i]));
    const double perimeter=cumulative.back();
    if(perimeter<=1e-9)return {};
    std::vector<Vec3> result;
    result.reserve(count);
    std::size_t edge_index=0;
    for(std::size_t i=0;i<count;++i) {
        const double target=perimeter*static_cast<double>(i)/static_cast<double>(count);
        while(edge_index+1<loop.size()&&cumulative[edge_index+1]<target)
            ++edge_index;
        const double edge_length=cumulative[edge_index+1]-cumulative[edge_index];
        const double t=edge_length>1e-12?(target-cumulative[edge_index])/edge_length:0.0;
        result.push_back(add(loop[edge_index],
            mul(sub(loop[(edge_index+1)%loop.size()],loop[edge_index]),t)));
    }
    return result;
}
bool bridge_regions(const Region& a,const Region& b,double gap,std::vector<Triangle>& out) {
    std::vector<Vec3> aa,bb;
    if(!get_boundary(a,aa)||!get_boundary(b,bb))return false;
    if(aa.size()>2048||bb.size()>2048)return false;

    // Find the best cyclic start and winding for the second loop. Resampling
    // is used only to score correspondence; the actual strip retains every
    // original boundary vertex, so its caps remain watertight.
    const std::size_t sample_count=std::min<std::size_t>(512,std::max(aa.size(),bb.size()));
    const std::vector<Vec3> sample_a=resample_loop(aa,sample_count);
    double best=std::numeric_limits<double>::infinity();
    std::vector<Vec3> matched_loop;
    for(int dir=0;dir<2;++dir) {
        std::vector<Vec3> oriented=bb;
        if(dir!=0)std::reverse(oriented.begin(),oriented.end());
        for(std::size_t shift=0;shift<oriented.size();++shift) {
            std::vector<Vec3> candidate=oriented;
            std::rotate(candidate.begin(),candidate.begin()+static_cast<std::ptrdiff_t>(shift),candidate.end());
            const std::vector<Vec3> samples=resample_loop(candidate,sample_count);
            double score=0.0;
            for(std::size_t i=0;i<sample_count;++i) {
                const Vec3 d=sub(sample_a[i],samples[i]);
                score+=dot(d,d);
            }
            if(score<best) {
                best=score;
                matched_loop=std::move(candidate);
            }
        }
    }
    if(matched_loop.size()!=bb.size())return false;
    bb=std::move(matched_loop);

    const Vec3 oa=mul(a.normal,gap),ob=mul(b.normal,gap);
    for(auto& v:aa)v=add(v,oa);
    for(auto& v:bb)v=add(v,ob);

    const Vec3 axis=unit(sub(b.center,a.center));
    const Vec3 midcenter=mul(add(a.center,b.center),.5);
    auto append_side=[&](Triangle tri) {
        const Vec3 mid=mul(add(add(tri.a,tri.b),tri.c),1.0/3.0);
        const Vec3 rel=sub(mid,midcenter);
        const Vec3 radial=sub(rel,mul(axis,dot(rel,axis)));
        if(dot(cross(sub(tri.b,tri.a),sub(tri.c,tri.a)),radial)<0)
            std::swap(tri.b,tri.c);
        add_triangle_if_valid(out,tri.a,tri.b,tri.c);
    };

    // Zipper triangulation supports different boundary vertex counts without
    // introducing T-junctions between the generated side walls and end caps.
    // Advance by normalized perimeter distance, not vertex index, so uneven
    // mesh tessellation does not twist the bridge strip.
    const std::vector<double> fractions_a=normalized_loop_fractions(aa);
    const std::vector<double> fractions_b=normalized_loop_fractions(bb);
    if(fractions_a.back()<=0.0||fractions_b.back()<=0.0)return false;
    std::size_t i=0,j=0;
    while(i<aa.size()||j<bb.size()) {
        const double next_a=i<aa.size()?fractions_a[i+1]:std::numeric_limits<double>::infinity();
        const double next_b=j<bb.size()?fractions_b[j+1]:std::numeric_limits<double>::infinity();
        const Vec3 a0=aa[i%aa.size()];
        const Vec3 b0=bb[j%bb.size()];
        if(std::abs(next_a-next_b)<1e-12) {
            const Vec3 a1=aa[(i+1)%aa.size()];
            const Vec3 b1=bb[(j+1)%bb.size()];
            append_side({a0,a1,b1});
            append_side({a0,b1,b0});
            ++i;++j;
        } else if(next_a<next_b) {
            const Vec3 a1=aa[(i+1)%aa.size()];
            append_side({a0,a1,b0});
            ++i;
        } else {
            const Vec3 b1=bb[(j+1)%bb.size()];
            append_side({a0,b1,b0});
            ++j;
        }
    }

    append_cap(out,a,oa);
    append_cap(out,b,ob);
    return true;
}
}
ConnectorResult generate_connectors(const std::vector<Triangle>& painted,
    const ConnectorOptions& options) {
    auto regions=connected_painted_regions(painted);
    ConnectorResult result;
    result.regions=regions.size();
    std::vector<bool> paired(regions.size(),false);
    struct Pair {double distance;std::size_t a,b;};
    std::vector<Pair> candidates;
    for(std::size_t i=0;i<regions.size();++i)
        for(std::size_t j=i+1;j<regions.size();++j){
            Vec3 delta=sub(regions[j].center,regions[i].center);
            const double distance=len(delta);
            if(distance<=2*options.fit_gap_mm+.01 ||
               distance>std::max(0.0,options.max_bridge_distance_mm))continue;
            const double min_area=std::min(regions[i].area,regions[j].area);
            const double max_area=std::max(regions[i].area,regions[j].area);
            if(min_area<=1e-9 ||
               max_area/min_area>std::max(1.0,options.max_bridge_area_ratio))continue;
            const Vec3 direction=unit(delta);
            if(dot(regions[i].normal,direction)>.45&&
               dot(regions[j].normal,mul(direction,-1))>.45)
                candidates.push_back({distance,i,j});
        }
    std::sort(candidates.begin(),candidates.end(),[](const Pair& a,const Pair& b){
        return a.distance<b.distance;
    });
    for(const Pair& p:candidates) {
        if(paired[p.a]||paired[p.b])continue;
        std::vector<Triangle> bridge;
        if(!bridge_regions(regions[p.a],regions[p.b],
            std::max(0.0,options.fit_gap_mm),bridge) ||
           !is_closed_oriented_volume(bridge))continue;
        result.triangles.insert(result.triangles.end(),bridge.begin(),bridge.end());
        paired[p.a]=paired[p.b]=true;
        ++result.bridges;
    }
    for(std::size_t i=0;i<regions.size();++i)if(!paired[i]) {
        const auto down=generate_downward_connectors(regions[i].facets,options);
        result.triangles.insert(result.triangles.end(),down.triangles.begin(),down.triangles.end());
        result.downward_connectors+=down.downward_connectors;
        result.skipped_regions+=down.skipped_regions;
    }
    return result;
}

} // namespace Slic3r::Connector
