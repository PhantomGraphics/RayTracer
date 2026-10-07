#include "PbvrResampler.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <unordered_map>

namespace Phantom::RayTracer {
namespace {
using V=Math::Vector3dd;
bool finite(V p) { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z); }
std::uint64_t mix(std::uint64_t x) {
    x=(x^(x>>30))*0xbf58476d1ce4e5b9ull;
    x=(x^(x>>27))*0x94d049bb133111ebull;
    return x^(x>>31);
}
struct Random {
    std::mt19937 engine;
    explicit Random(std::uint32_t seed):engine(seed) {}
    double next() { return (static_cast<double>(engine())+0.5)/4294967296.0; }
};
struct Cell {
    int surface;
    std::int64_t x,y,z;
    bool operator==(const Cell& b) const { return surface==b.surface && x==b.x && y==b.y && z==b.z; }
};
struct CellHash {
    std::size_t operator()(const Cell& c) const {
        return static_cast<std::size_t>(mix(static_cast<std::uint64_t>(c.x))
            ^mix(static_cast<std::uint64_t>(c.y)+0x9e3779b97f4a7c15ull)
            ^mix(static_cast<std::uint64_t>(c.z)+0x243f6a8885a308d3ull)^mix(c.surface));
    }
};
struct Group { std::vector<std::size_t> members; double weight=0; std::size_t quota=0; };

// Store the actual representable CDF intervals and use those intervals for
// compensation. This also checks numerically lost support when uniformMix=0.
bool distribution(const std::vector<double>& weights,double uniformMix,std::vector<double>& cdf) {
    double total=0; for(double w:weights) total+=w;
    if(!std::isfinite(total) || weights.empty() || total<0 || (total==0 && uniformMix==0)) return false;
    cdf.resize(weights.size()); double cumulative=0;
    for(std::size_t i=0;i<weights.size();++i) {
        cumulative+=total>0?(1-uniformMix)*(weights[i]/total)+uniformMix/weights.size():1.0/weights.size(); cdf[i]=cumulative;
    }
    for(auto& p:cdf) p/=cumulative;
    cdf.back()=1;
    double previous=0;
    for(double p:cdf) { if(p<=previous || !std::isfinite(p)) return false; previous=p; }
    return true;
}
double probability(const std::vector<double>& cdf,std::size_t index)
{ return cdf[index]-(index?cdf[index-1]:0); }
std::size_t pick(const std::vector<double>& cdf,double u,std::size_t& cursor) {
    while(cursor+1<cdf.size() && u>=cdf[cursor]) ++cursor;
    return cursor;
}
}
bool resamplePbvr(const std::vector<PbvrArrival>& input,const PbvrPhotonSettings& cfg,
    std::uint32_t seed,std::vector<PbvrSelectedSample>& selected,PbvrResampleStats& stats,std::string& error)
{
    selected.clear(); stats={}; error.clear();
    const auto fail=[&](const char* message) { selected.clear(); stats={}; error=message; return false; };
    if((cfg.selection!=PbvrSelection::Power && cfg.selection!=PbvrSelection::SpatialPower)
        || !std::isfinite(cfg.retention) || cfg.retention<=0 || cfg.retention>1
        || !std::isfinite(cfg.uniformMix) || cfg.uniformMix<0 || cfg.uniformMix>1
        || (cfg.selection==PbvrSelection::SpatialPower && (!std::isfinite(cfg.spatialCellSize) || cfg.spatialCellSize<=0)))
        return fail("Invalid PBVR resampling settings");
    if(input.empty()) return true;
    if(input.size()>static_cast<std::size_t>(std::numeric_limits<int>::max())) return fail("Too many PBVR arrivals");
    double scale=0;
    for(const auto& p:input) {
        if(!finite(p.position) || !finite(p.power) || p.surface<0
            || std::min({p.power.x,p.power.y,p.power.z})<0 || std::max({p.power.x,p.power.y,p.power.z})<=0)
            return fail("Invalid PBVR arrival");
        scale=std::max({scale,p.power.x,p.power.y,p.power.z});
    }
    const auto target=static_cast<std::size_t>(std::ceil(input.size()*cfg.retention));
    const std::size_t count=cfg.maxParticles?std::min(target,cfg.maxParticles):target;
    std::vector<PbvrSelectedSample> result; result.reserve(count);
    if(count==input.size()) {
        for(std::size_t i=0;i<input.size();++i) result.push_back({i,1,0,false});
        selected=std::move(result); return true;
    }
    std::vector<double> weights; weights.reserve(input.size());
    for(const auto& p:input) weights.push_back(glm::dot(p.power/scale,V(0.2126,0.7152,0.0722)));
    std::vector<Group> groups;
    if(cfg.selection==PbvrSelection::Power) {
        groups.resize(1); groups[0].members.reserve(input.size());
        for(std::size_t i=0;i<input.size();++i) { groups[0].members.push_back(i); groups[0].weight+=weights[i]; }
    } else {
        std::unordered_map<Cell,std::size_t,CellHash> cells;
        cells.reserve(std::min<std::size_t>(input.size(),65536));
        for(std::size_t i=0;i<input.size();++i) {
            const V q=glm::floor(input[i].position/cfg.spatialCellSize);
            constexpr double limit=9223372036854775808.0;
            if(!finite(q) || std::min({q.x,q.y,q.z})<-limit || std::max({q.x,q.y,q.z})>=limit)
                return fail("PBVR spatial cell coordinate outside int64 range");
            const Cell cell{input[i].surface,static_cast<std::int64_t>(q.x),static_cast<std::int64_t>(q.y),static_cast<std::int64_t>(q.z)};
            const auto found=cells.find(cell); std::size_t g;
            if(found==cells.end()) { g=groups.size(); cells.emplace(cell,g); groups.emplace_back(); }
            else g=found->second;
            groups[g].members.push_back(i); groups[g].weight+=weights[i];
        }
    }
    stats.strata=groups.size(); Random rng(seed);
    std::vector<double> groupWeights; groupWeights.reserve(groups.size());
    for(const auto& g:groups) groupWeights.push_back(g.weight);
    std::vector<double> groupCdf;
    if(!distribution(groupWeights,cfg.uniformMix,groupCdf)) return fail("PBVR group probability lost support");
    const bool coverAll=count>=groups.size();
    if(coverAll) for(auto& g:groups) g.quota=1;
    const std::size_t remaining=coverAll?count-groups.size():count;
    std::size_t cursor=0;
    for(std::size_t i=0;i<remaining;++i) {
        const double u=(i+rng.next())/remaining;
        ++groups[pick(groupCdf,u,cursor)].quota;
    }
    std::vector<double> localWeights,localCdf;
    for(std::size_t g=0;g<groups.size();++g) {
        const auto& group=groups[g]; if(!group.quota) continue;
        ++stats.sampledStrata;
        localWeights.clear(); localWeights.reserve(group.members.size());
        for(auto i:group.members) localWeights.push_back(weights[i]);
        if(!distribution(localWeights,cfg.uniformMix,localCdf)) return fail("PBVR local probability lost support");
        cursor=0;
        for(std::size_t k=0;k<group.quota;++k) {
            const double u=(k+rng.next())/group.quota;
            const std::size_t local=pick(localCdf,u,cursor),source=group.members[local];
            // If every stratum is covered, estimate its total conditional on its
            // quota. Otherwise account for the probability of selecting its group.
            const double expected=coverAll?static_cast<double>(group.quota):count*probability(groupCdf,g);
            const double multiplier=1/(expected*probability(localCdf,local));
            if(!std::isfinite(multiplier)) return fail("PBVR importance weight overflow");
            const std::uint64_t key=mix((static_cast<std::uint64_t>(seed)<<32)
                ^static_cast<std::uint64_t>(result.size())^0xd1b54a32d192ed03ull);
            result.push_back({source,multiplier,key,true});
        }
    }
    if(result.size()!=count) return fail("PBVR resampling count mismatch");
    selected=std::move(result); return true;
}
} // namespace Phantom::RayTracer
