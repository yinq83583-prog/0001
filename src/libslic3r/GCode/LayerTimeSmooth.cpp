#include "LayerTimeSmooth.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <regex>
#include <sstream>
#include <utility>

namespace Slic3r::GCode {
namespace {
struct Move {
    std::size_t line = 0;
    double duration = 0.0;
    double feed_mm_min = 0.0;
    double floor = 1.0;
    bool eligible = false;
};
struct Layer {
    std::size_t begin = 0, end = 0;
    double total = 0.0, fixed = 0.0, adjustable = 0.0, max_adjustable = 0.0;
    std::vector<Move> moves;
};
double clampv(double x, double a, double b) { return std::max(a, std::min(b, x)); }

bool marker(const std::string& s) {
    static const std::regex r(R"(^\s*;\s*(?:LAYER_CHANGE\b|CHANGE_LAYER\b|LAYER\s*:\s*-?\d+))", std::regex::icase);
    return std::regex_search(s, r);
}
std::string code_part(const std::string& s) { return s.substr(0, s.find(';')); }
std::string comment_part(const std::string& s) {
    const auto p=s.find(';'); return p==std::string::npos ? std::string() : s.substr(p);
}
std::string command(const std::string& s) {
    std::istringstream in(s); std::string c; in>>c;
    std::transform(c.begin(),c.end(),c.begin(),[](unsigned char x){return char(std::toupper(x));});
    return c;
}
std::map<char,double> words(const std::string& s) {
    static const std::regex r(R"(([A-Za-z])\s*(-?(?:\d+(?:\.\d*)?|\.\d+)))");
    std::map<char,double> out;
    for(std::sregex_iterator i(s.begin(),s.end(),r),e;i!=e;++i)
        out[char(std::toupper((*i)[1].str()[0]))]=std::strtod((*i)[2].str().c_str(),nullptr);
    return out;
}
bool allowed(const std::string& role,const LayerTimeSmoothConfig& c) {
    std::string r=role;
    std::transform(r.begin(),r.end(),r.begin(),[](unsigned char x){return char(std::tolower(x));});
    if(r.find("prime tower")!=r.npos||r.find("wipe tower")!=r.npos||r.find("purge")!=r.npos)return false;
    if(r.find("bridge")!=r.npos||r.find("overhang")!=r.npos)return c.adjust_bridges;
    if(r.find("support")!=r.npos)return c.adjust_support;
    if(r.find("gap fill")!=r.npos||r.find("gapfill")!=r.npos)return c.adjust_gap_fill;
    if(r.find("top surface")!=r.npos||r.find("top solid")!=r.npos||r.find("ironing")!=r.npos)return c.adjust_top_surface;
    if(r.find("outer wall")!=r.npos||r.find("external perimeter")!=r.npos)return !c.preserve_outer_walls;
    if(r.find("inner wall")!=r.npos||r.find("internal perimeter")!=r.npos||r.find("perimeter")!=r.npos)return c.adjust_inner_walls;
    if(r.find("infill")!=r.npos||r.find("bottom surface")!=r.npos)return c.adjust_infill;
    return false;
}
std::vector<double> gaussian(const std::vector<double>& v,int radius) {
    if(radius<=1)return v;
    const double sigma=std::max(.65,double(radius)/2.0);
    const int half=std::max(1,int(std::ceil(radius*1.5)));
    std::vector<double> out(v.size(),1.0);
    for(std::size_t i=0;i<v.size();++i){double sum=0,ws=0;
        const int lo=std::max(0,int(i)-half),hi=std::min(int(v.size())-1,int(i)+half);
        for(int j=lo;j<=hi;++j){const double d=j-int(i),w=std::exp(-(d*d)/(2*sigma*sigma));sum+=v[j]*w;ws+=w;}
        out[i]=sum/std::max(ws,1e-12);
    } return out;
}
double profile_at(const std::vector<double>& p,double x) {
    if(p.empty())return 1.0;
    x=clampv(x,0,1)*(p.size()-1);const auto a=std::size_t(x),b=std::min(p.size()-1,a+1);
    return clampv(p[a]*(1-(x-a))+p[b]*(x-a),0,1);
}
std::string replace_feed(const std::string& code,double f) {
    static const std::regex fr(R"((?i)(?<![A-Za-z])F\s*-?(?:\d+(?:\.\d*)?|\.\d+))");
    std::string out=std::regex_replace(code,fr,"");
    while(!out.empty()&&std::isspace(static_cast<unsigned char>(out.back())))out.pop_back();
    std::ostringstream ss;ss<<out<<" F"<<std::fixed;ss.precision(1);ss<<f;return ss.str();
}
}

bool LayerTimeSmooth::process(std::string& gcode,const LayerTimeSmoothConfig& input,LayerTimeSmoothReport* report) {
    LayerTimeSmoothReport rep;
    auto finish=[&](LayerTimeSmoothReport::Status s,const std::string& m,bool changed=false){rep.status=s;rep.message=m;if(report)*report=rep;return changed;};
    if(!input.enabled)return finish(LayerTimeSmoothReport::Status::Disabled,"Layer Time Smooth disabled.");
    if(gcode.find("; LAYER_TIME_SMOOTH_V521_NATIVE")!=std::string::npos)
        return finish(LayerTimeSmoothReport::Status::AlreadyProcessed,"G-code already processed; skipped.");
    LayerTimeSmoothConfig c=input;
    c.quality_speed=clampv(c.quality_speed,0,1);
    c.max_layer_variation_percent=10+40*c.quality_speed;
    c.max_slowdown_percent=clampv(c.max_slowdown_percent,0,70);
    c.min_speed_mm_s=clampv(c.min_speed_mm_s,1,100);
    c.max_total_time_increase_percent=clampv(c.max_total_time_increase_percent,0,50);
    c.smooth_radius=int(clampv(c.smooth_radius,1,20));
    if(c.manual_profile.size()!=64)c.manual_profile.assign(64,1.0);
    for(auto& x:c.manual_profile)x=clampv(x,0,1);

    std::vector<std::string> lines; std::istringstream input_stream(gcode);std::string line;
    while(std::getline(input_stream,line))lines.push_back(line);
    std::vector<std::size_t> starts;
    for(std::size_t i=0;i<lines.size();++i)if(marker(lines[i]))starts.push_back(i);
    if(starts.size()<3)return finish(LayerTimeSmoothReport::Status::NoLayers,"Fewer than three reliable layer markers; unchanged.");
    std::vector<Layer> layers(starts.size());
    for(std::size_t i=0;i<starts.size();++i){layers[i].begin=starts[i];layers[i].end=i+1<starts.size()?starts[i+1]:lines.size();}

    bool absxyz=true,abse=true;double unit=1,feed=1800,last_e=std::numeric_limits<double>::quiet_NaN();
    double x=std::numeric_limits<double>::quiet_NaN(),y=x,z=x;std::string role;
    std::vector<int> line_layer(lines.size(),-1);
    for(std::size_t li=0;li<layers.size();++li)for(std::size_t n=layers[li].begin;n<layers[li].end;++n)line_layer[n]=int(li);
    for(std::size_t n=0;n<lines.size();++n){
        const std::string code=code_part(lines[n]),com=comment_part(lines[n]);
        if(com.rfind(";TYPE:",0)==0)role=com.substr(6);
        else if(com.rfind("; FEATURE:",0)==0)role=com.substr(10);
        const std::string cmd=command(code);auto w=words(code);
        if(cmd=="G90"){absxyz=true;continue;}if(cmd=="G91"){absxyz=false;continue;}
        if(cmd=="M82"){abse=true;continue;}if(cmd=="M83"){abse=false;continue;}
        if(cmd=="G20"){unit=25.4;continue;}if(cmd=="G21"){unit=1;continue;}
        if(cmd=="G92"){if(w.count('X'))x=w['X']*unit;if(w.count('Y'))y=w['Y']*unit;if(w.count('Z'))z=w['Z']*unit;if(w.count('E'))last_e=w['E']*unit;continue;}
        if(cmd!="G0"&&cmd!="G00"&&cmd!="G1"&&cmd!="G01")continue;
        if(w.count('F'))feed=w['F']*unit;
        double nx=x,ny=y,nz=z;
        auto axis=[&](char a,double old){if(!w.count(a))return old;double v=w[a]*unit;return absxyz||!std::isfinite(old)?v:old+v;};
        nx=axis('X',x);ny=axis('Y',y);nz=axis('Z',z);
        double dist=0;if(std::isfinite(x)&&std::isfinite(y)&&std::isfinite(nx)&&std::isfinite(ny))
            dist=std::sqrt((nx-x)*(nx-x)+(ny-y)*(ny-y)+(std::isfinite(z)&&std::isfinite(nz)?(nz-z)*(nz-z):0));
        x=nx;y=ny;z=nz;
        bool extrusion=false;if(w.count('E')){double ev=w['E']*unit,de=abse?(std::isfinite(last_e)?ev-last_e:ev):ev;last_e=abse?ev:last_e;extrusion=de>1e-8;}
        int li=line_layer[n];if(li<0||dist<=1e-8||feed<=1e-6)continue;
        Move m;m.line=n;m.feed_mm_min=feed;m.duration=dist/(feed/60.0);m.eligible=extrusion&&allowed(role,c);
        if(m.eligible){double minfeed=std::max(c.min_speed_mm_s*60.0,feed*(1-c.max_slowdown_percent/100.0));m.floor=clampv(std::min(feed,minfeed)/feed,.01,1);layers[li].adjustable+=m.duration;layers[li].max_adjustable+=m.duration/m.floor;}
        else layers[li].fixed+=m.duration;
        layers[li].total+=m.duration;layers[li].moves.push_back(m);
    }
    if(layers.size()<3)return finish(LayerTimeSmoothReport::Status::NoLayers,"No usable layer spans; unchanged.");
    std::vector<double> times,caps,target;times.reserve(layers.size());
    for(const auto& l:layers){times.push_back(std::max(l.total,.001));caps.push_back(l.adjustable>1e-9?l.fixed+l.max_adjustable:l.total);}
    const double ratio=1+c.max_layer_variation_percent/100.0;target=times;
    for(std::size_t pass=0;pass<layers.size()*2;++pass){bool changed=false;
        for(std::size_t i=0;i+1<target.size();++i)if(target[i]<target[i+1]/ratio){double v=std::min(caps[i],target[i+1]/ratio);if(v>target[i]+1e-7){target[i]=v;changed=true;}}
        for(std::size_t i=target.size()-1;i>0;--i)if(target[i]<target[i-1]/ratio){double v=std::min(caps[i],target[i-1]/ratio);if(v>target[i]+1e-7){target[i]=v;changed=true;}}
        if(!changed)break;
    }
    std::vector<double> extra(target.size());for(std::size_t i=0;i<target.size();++i)extra[i]=std::max(0.0,target[i]-times[i]);
    extra=gaussian(extra,c.smooth_radius);
    for(std::size_t i=0;i<target.size();++i)target[i]=std::min(caps[i],times[i]+extra[i]*profile_at(c.manual_profile,double(i)/std::max<std::size_t>(1,target.size()-1)));
    double base_total=0,added=0;for(std::size_t i=0;i<times.size();++i){base_total+=times[i];added+=target[i]-times[i];}
    const double budget=base_total*c.max_total_time_increase_percent/100.0;
    if(added>budget+1e-6&&added>0){double scale=budget/added;for(std::size_t i=0;i<target.size();++i)target[i]=times[i]+(target[i]-times[i])*scale;}
    std::vector<double> scales(layers.size(),1.0);
    for(std::size_t i=0;i<layers.size();++i){const auto& l=layers[i];if(l.adjustable<=1e-9||target[i]<=times[i]+1e-8)continue;
        const double desired=std::max(l.adjustable,target[i]-l.fixed);double lo=.01,hi=1;
        for(int k=0;k<40;++k){double mid=(lo+hi)/2,est=0;for(const auto& m:l.moves)if(m.eligible)est+=m.duration/std::max(mid,m.floor);if(est>desired)lo=mid;else hi=mid;}
        scales[i]=(lo+hi)/2;
    }
    std::vector<std::string> out=lines;double output_feed=1800;std::size_t changed_count=0,restored=0;
    // Re-emit only explicit F words on eligible extrusion lines; inject an F restore
    // on the first following non-eligible move if modal feed would otherwise leak.
    for(std::size_t li=0;li<layers.size();++li)for(const auto& m:layers[li].moves){
        const double desired=m.eligible?std::max(m.feed_mm_min*scales[li],m.feed_mm_min*(1-c.max_slowdown_percent/100.0)):m.feed_mm_min;
        if(std::abs(desired-m.feed_mm_min)>.05){out[m.line]=replace_feed(code_part(lines[m.line]),desired/unit)+comment_part(lines[m.line]);++changed_count;}
        if(!m.eligible&&std::abs(desired-output_feed)>.05&&code_part(lines[m.line]).find('F')==std::string::npos){
            out[m.line]=replace_feed(code_part(lines[m.line]),desired/unit)+comment_part(lines[m.line]);++restored;
        }
        output_feed=desired;
    }
    if(changed_count==0)return finish(LayerTimeSmoothReport::Status::NoChange,"No eligible extrusion moves could be safely adjusted.");
    std::ostringstream result;for(const auto& s:out)result<<s<<'\n';
    result<<"; LAYER_TIME_SMOOTH_V521_NATIVE\n";
    if(c.write_layer_report){result<<"; LAYER_TIME_SMOOTH_REPORT: layers="<<layers.size()<<", changed_moves="<<changed_count<<"\n";}
    gcode=result.str();rep.layers=layers.size();rep.changed_moves=changed_count;rep.restored_moves=restored;rep.time_before=base_total;
    rep.time_after=base_total+added;rep.effective_layer_ratio=ratio;rep.scale_profile=scales;
    return finish(LayerTimeSmoothReport::Status::Processed,"Native Layer Time Smooth V5.2.1 processed.",true);
}
} // namespace Slic3r::GCode
