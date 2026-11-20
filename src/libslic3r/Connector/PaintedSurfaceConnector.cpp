#include "PaintedSurfaceConnector.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
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
void add_quad(std::vector<Triangle>& tris,Vec3 a,Vec3 b,Vec3 c,Vec3 d){
    tris.push_back({a,b,c});tris.push_back({a,c,d});
}
}
std::vector<Region> connected_painted_regions(const std::vector<Triangle>& painted) {
    if(painted.empty())return {};
    DisjointSet ds(painted.size());
    std::map<Edge,std::size_t> owners;
    for(std::size_t i=0;i<painted.size();++i){
        const auto& t=painted[i];
        for(const Edge e:{edge(t.a,t.b),edge(t.b,t.c),edge(t.c,t.a)}){
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
            for(auto [a,b]:{std::pair<Vec3,Vec3>{t.a,t.b},{t.b,t.c},{t.c,t.a}}){
                const Edge e=edge(a,b);
                if(borders.count(e))duplicate.insert(e);
                else borders.emplace(e,std::make_pair(a,b));
            }
        }
        const Vec3 offset=mul(r.normal,gap);
        // Conformal cap: preserve the actual painted triangles and their winding.
        for(const Triangle& t:r.facets){
            const Triangle top{add(t.a,offset),add(t.b,offset),add(t.c,offset)};
            result.triangles.push_back(top);
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
            add_quad(result.triangles,a,b,c,d);
        }
        ++result.downward_connectors;
    }
    return result;
}
} // namespace Slic3r::Connector
