#include "PaintedSurfaceConnector.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

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
        if(r.center.z<=options.bed_z_mm+std::max(0.0,options.min_height_mm)){
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
        // Conformal cap: preserve the actual painted triangles and their winding.
        for(const Triangle& t:r.facets){
            const Triangle top{add(t.a,offset),add(t.b,offset),add(t.c,offset)};
            add_triangle_if_valid(result.triangles,top.a,top.b,top.c);
            Triangle bottom=top;
            bottom.a.z=options.bed_z_mm;bottom.b.z=options.bed_z_mm;bottom.c.z=options.bed_z_mm;
            std::swap(bottom.b,bottom.c);
            if(len(cross(sub(bottom.b,bottom.a),sub(bottom.c,bottom.a)))>1e-12)
                result.triangles.push_back(bottom);
        }
        // Only boundary edges receive walls; shared triangle edges are internal.
        for(const auto& item:borders){
            if(duplicate.count(item.first))continue;
            Vec3 a=add(item.second.first,offset),b=add(item.second.second,offset);
            Vec3 c=b,d=a;c.z=options.bed_z_mm;d.z=options.bed_z_mm;
            add_quad(result.triangles,b,a,d,c);
        }
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
bool bridge_regions(const Region& a,const Region& b,double gap,std::vector<Triangle>& out) {
    std::vector<Vec3> aa,bb;
    if(!get_boundary(a,aa)||!get_boundary(b,bb)||aa.size()!=bb.size())return false;
    if(aa.size()>4096)return false;
    const Vec3 oa=mul(a.normal,gap),ob=mul(b.normal,gap);
    for(auto& v:aa)v=add(v,oa);
    for(auto& v:bb)v=add(v,ob);
    double best=std::numeric_limits<double>::infinity();
    std::size_t shift=0;bool reverse=false;
    // Choose nearest cyclic correspondence; allow opposite winding.
    for(int dir=0;dir<2;++dir)
        for(std::size_t offset=0;offset<bb.size();++offset){
            double score=0;
            for(std::size_t i=0;i<aa.size();++i){
                auto j=dir==0?(i+offset)%bb.size():(offset+bb.size()-i)%bb.size();
                const Vec3 d=sub(aa[i],bb[j]);score+=dot(d,d);
            }
            if(score<best){best=score;shift=offset;reverse=dir!=0;}
        }
    std::vector<Vec3> matched(bb.size());
    for(std::size_t i=0;i<bb.size();++i){
        auto j=reverse?(shift+bb.size()-i)%bb.size():(i+shift)%bb.size();
        matched[i]=bb[j];
    }
    // Keep faces consistently outward using a radial normal relative to
    // the bridge axis rather than relying on two source windings agreeing.
    const Vec3 axis=unit(sub(b.center,a.center));
    for(std::size_t i=0;i<aa.size();++i){
        std::size_t j=(i+1)%aa.size();
        const Vec3 mid=mul(add(add(aa[i],aa[j]),add(matched[i],matched[j])),.25);
        const Vec3 midcenter=mul(add(a.center,b.center),.5);
        const Vec3 rel=sub(mid,midcenter);
        const Vec3 radial=sub(rel,mul(axis,dot(rel,axis)));
        Triangle first{aa[i],aa[j],matched[j]};
        if(dot(cross(sub(first.b,first.a),sub(first.c,first.a)),radial)<0)
            std::swap(first.b,first.c);
        Triangle second{aa[i],matched[j],matched[i]};
        if(dot(cross(sub(second.b,second.a),sub(second.c,second.a)),radial)<0)
            std::swap(second.b,second.c);
        out.push_back(first);out.push_back(second);
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
            std::max(0.0,options.fit_gap_mm),bridge))continue;
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
