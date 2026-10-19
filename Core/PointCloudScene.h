#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace Phantom::RayTracer {

// Point clouds in the path tracer (docs/todo/PLAN_phantomstudio_pointcloud.md Phase 4). A point is a sphere of its radius in the
// cloud's local space; the cloud's model matrix maps that space to the world, so a non-uniform scale gives ellipsoids exactly. The
// points are never expanded into triangles: the tracer intersects the spheres analytically through a BVH over the points, in the
// cloud's local space, and the cloud as a whole is one object of the scene's own BVH (so shadows, reflections and occlusion between
// points and meshes all work through the ordinary world.hit()).

// A bounding volume hierarchy over spheres (centre, radius), in the space the spheres are given in. Immutable once built.
class PointBvh {
public:
    struct Hit {
        uint32_t index = 0;   ///< the point
        double   t = 0.0;     ///< ray parameter: origin + t * direction (the direction need not be normalised)
    };

    /// @brief Builds over `count` points: positions xyz interleaved, radii one per point. O(n log n), single-threaded.
    PointBvh(const float* positions, const float* radii, size_t count);

    /// @brief The nearest sphere hit with tMin < t < tMax, or false. Every radius is multiplied by `radiusScale` (the BVH bounds are
    /// widened accordingly, so a scale above 1 still finds every sphere). `positions` / `radii` must be the arrays it was built from.
    bool closestHit(const float* positions, const float* radii, const double origin[3], const double direction[3],
        double tMin, double tMax, double radiusScale, Hit& hit) const;

    size_t pointCount() const { return count_; }
    size_t nodeCount() const { return nodes_.size(); }
    float  maxRadius() const { return maxRadius_; }
    /// Bounds of all the spheres at scale 1 (min xyz, max xyz); all zero for an empty set.
    const float* boundsMin() const { return rootMin_; }
    const float* boundsMax() const { return rootMax_; }

private:
    struct Node {
        float bmin[3], bmax[3];
        uint32_t first = 0;   // leaf: first index into order_; inner: index of the right child (the left child is the next node)
        uint32_t count = 0;   // leaf: number of points; inner: 0
    };
    std::vector<Node> nodes_;
    std::vector<uint32_t> order_;
    size_t count_ = 0;
    float maxRadius_ = 0.f;
    float rootMin_[3] = {0, 0, 0}, rootMax_[3] = {0, 0, 0};
};

/// The immutable points of a cloud, shared by every render that uses them -- the viewport's trace window, a still image, each frame
/// of a sequence and every duplicate of the object -- so the BVH is built once per point array, not once per render. The arrays are
/// not copied: `owner` keeps whatever holds them alive.
class RtPointCloudData {
public:
    RtPointCloudData(const float* positions, const float* colors, const float* radii, size_t count, std::shared_ptr<const void> owner);

    const float* positions() const { return positions_; }
    const float* colors() const { return colors_; }   ///< linear RGB per point; may be null (then white)
    const float* radii() const { return radii_; }
    size_t count() const { return count_; }

    /// The BVH, built on first use (thread-safe: concurrent first users wait for one build).
    const PointBvh& bvh() const;
    /// Wall time the BVH build took, in milliseconds; 0 until it has been built.
    double bvhBuildMilliseconds() const { return buildMs_; }

private:
    const float* positions_;
    const float* colors_;
    const float* radii_;
    size_t count_;
    std::shared_ptr<const void> owner_;
    mutable std::once_flag once_;
    mutable std::unique_ptr<PointBvh> bvh_;
    mutable double buildMs_ = 0.0;
};

/// One point cloud object of a scene.
struct RtPointCloud {
    std::shared_ptr<const RtPointCloudData> data;
    double modelToWorld[16] = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1 };   ///< column-major 4x4 (translation in elements 12..14)
    double tint[3] = { 1.0, 1.0, 1.0 };      ///< the object's base colour: multiplies the point colour
    bool   useUniformColor = false;          ///< true: every point has `uniformColor` instead of its own colour
    double uniformColor[3] = { 0.8, 0.8, 0.8 };
    double radiusScale = 1.0;
    double metallic = 0.0;
    double roughness = 0.5;
};

}  // namespace Phantom::RayTracer
