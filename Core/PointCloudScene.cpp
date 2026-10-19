#include "PointCloudScene.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace Phantom::RayTracer {

namespace {

constexpr uint32_t kLeafSize = 6;

}  // namespace

PointBvh::PointBvh(const float* positions, const float* radii, size_t count) : count_(count)
{
    if (count == 0 || !positions || !radii) { count_ = 0; return; }
    order_.resize(count);
    for (size_t i = 0; i < count; ++i) order_[i] = static_cast<uint32_t>(i);
    nodes_.reserve(2 * (count / kLeafSize + 1));
    for (size_t i = 0; i < count; ++i) maxRadius_ = std::max(maxRadius_, radii[i]);

    // Recursive median split on the longest axis of the centroids; the left child is always the next node.
    // Depth is about log2(n / leaf) (balanced by the median), so plain recursion is safe.
    auto build = [&](auto&& self, uint32_t begin, uint32_t end) -> uint32_t {
        const uint32_t nodeIndex = static_cast<uint32_t>(nodes_.size());
        nodes_.emplace_back();
        float bmin[3] = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
        float bmax[3] = { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
        float cmin[3] = { bmin[0], bmin[1], bmin[2] }, cmax[3] = { bmax[0], bmax[1], bmax[2] };
        for (uint32_t k = begin; k < end; ++k) {
            const uint32_t i = order_[k];
            const float r = radii[i];
            for (int a = 0; a < 3; ++a) {
                const float c = positions[3 * static_cast<size_t>(i) + a];
                bmin[a] = std::min(bmin[a], c - r);
                bmax[a] = std::max(bmax[a], c + r);
                cmin[a] = std::min(cmin[a], c);
                cmax[a] = std::max(cmax[a], c);
            }
        }
        for (int a = 0; a < 3; ++a) { nodes_[nodeIndex].bmin[a] = bmin[a]; nodes_[nodeIndex].bmax[a] = bmax[a]; }

        const uint32_t n = end - begin;
        int axis = 0;
        float extent = cmax[0] - cmin[0];
        for (int a = 1; a < 3; ++a) if (cmax[a] - cmin[a] > extent) { extent = cmax[a] - cmin[a]; axis = a; }
        if (n <= kLeafSize || !(extent > 0.f)) {
            nodes_[nodeIndex].first = begin;
            nodes_[nodeIndex].count = n;
            return nodeIndex;
        }
        const uint32_t mid = begin + n / 2;
        std::nth_element(order_.begin() + begin, order_.begin() + mid, order_.begin() + end,
            [&](uint32_t x, uint32_t y) { return positions[3 * static_cast<size_t>(x) + axis] < positions[3 * static_cast<size_t>(y) + axis]; });
        self(self, begin, mid);
        const uint32_t right = self(self, mid, end);
        nodes_[nodeIndex].first = right;
        nodes_[nodeIndex].count = 0;
        return nodeIndex;
    };
    build(build, 0, static_cast<uint32_t>(count));
    for (int a = 0; a < 3; ++a) { rootMin_[a] = nodes_[0].bmin[a]; rootMax_[a] = nodes_[0].bmax[a]; }
}

bool PointBvh::closestHit(const float* positions, const float* radii, const double origin[3], const double direction[3],
    double tMin, double tMax, double radiusScale, Hit& hit) const
{
    if (nodes_.empty()) return false;
    const double inf = std::numeric_limits<double>::infinity();
    double invDir[3];
    for (int a = 0; a < 3; ++a) invDir[a] = direction[a] != 0.0 ? 1.0 / direction[a] : inf;
    const double a2 = direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2];
    if (!(a2 > 0.0)) return false;
    const double inflate = radiusScale > 1.0 ? (radiusScale - 1.0) * static_cast<double>(maxRadius_) : 0.0;

    double best = tMax;
    bool found = false;
    uint32_t stack[96];
    int top = 0;
    stack[top++] = 0;

    auto boxEntry = [&](const Node& node, double& tNear) {
        double t0 = tMin, t1 = best;
        for (int a = 0; a < 3; ++a) {
            double lo = (static_cast<double>(node.bmin[a]) - inflate - origin[a]) * invDir[a];
            double hi = (static_cast<double>(node.bmax[a]) + inflate - origin[a]) * invDir[a];
            if (std::isnan(lo)) lo = -inf;   // 0 * inf: the origin is on the slab plane
            if (std::isnan(hi)) hi = inf;
            if (lo > hi) std::swap(lo, hi);
            t0 = std::max(t0, lo);
            t1 = std::min(t1, hi);
            if (t0 > t1) return false;
        }
        tNear = t0;
        return true;
    };

    while (top > 0) {
        const uint32_t index = stack[--top];
        const Node& node = nodes_[index];
        double entry = 0.0;
        if (!boxEntry(node, entry)) continue;
        if (node.count > 0) {
            for (uint32_t k = node.first; k < node.first + node.count; ++k) {
                const uint32_t i = order_[k];
                const double r = static_cast<double>(radii[i]) * radiusScale;
                const double oc[3] = { origin[0] - positions[3 * static_cast<size_t>(i)],
                                       origin[1] - positions[3 * static_cast<size_t>(i) + 1],
                                       origin[2] - positions[3 * static_cast<size_t>(i) + 2] };
                const double halfB = oc[0] * direction[0] + oc[1] * direction[1] + oc[2] * direction[2];
                const double c = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - r * r;
                const double disc = halfB * halfB - a2 * c;
                if (disc < 0.0) continue;
                const double s = std::sqrt(disc);
                double t = (-halfB - s) / a2;
                if (!(t > tMin && t < best)) {
                    t = (-halfB + s) / a2;
                    if (!(t > tMin && t < best)) continue;
                }
                best = t;
                hit.index = i;
                hit.t = t;
                found = true;
            }
            continue;
        }
        // Inner node: visit the nearer child first so `best` shrinks early and prunes the other one.
        const uint32_t left = index + 1, right = node.first;
        double tl = 0.0, tr = 0.0;
        const bool hl = boxEntry(nodes_[left], tl), hr = boxEntry(nodes_[right], tr);
        if (top + 2 > 96) continue;   // a pathological depth: the median split makes this unreachable
        if (hl && hr) {
            if (tl <= tr) { stack[top++] = right; stack[top++] = left; }
            else          { stack[top++] = left;  stack[top++] = right; }
        } else if (hl) stack[top++] = left;
        else if (hr) stack[top++] = right;
    }
    return found;
}

RtPointCloudData::RtPointCloudData(const float* positions, const float* colors, const float* radii, size_t count,
    std::shared_ptr<const void> owner)
    : positions_(positions), colors_(colors), radii_(radii), count_(count), owner_(std::move(owner))
{
}

const PointBvh& RtPointCloudData::bvh() const
{
    std::call_once(once_, [this] {
        const auto start = std::chrono::steady_clock::now();
        bvh_ = std::make_unique<PointBvh>(positions_, radii_, count_);
        buildMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    });
    return *bvh_;
}

}  // namespace Phantom::RayTracer
