#include "pch.h"
#include "PathTracer.h"

#include "../../CGLib/Math/Vector3d.h"
#include "../../CGLib/Space/Space/BVH.h"

namespace Phantom::RayTracer {

using Vec3 = Math::Vector3dd;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kInfinity = std::numeric_limits<double>::infinity();

void reportProgress(const int y, const int height, int& nextPercent)
{
    const int donePercent = static_cast<int>((100.0 * static_cast<double>(y + 1)) / static_cast<double>(height));
    if (donePercent >= nextPercent) {
        std::cout << "  " << nextPercent << "%" << std::endl;
        nextPercent += 10;
    }
}

class Random {
public:
    explicit Random(std::uint32_t seed)
        : m_engine(seed), m_dist01(0.0, 1.0), m_dist11(-1.0, 1.0) {}

    double next01() { return m_dist01(m_engine); }

    double nextSigned() { return m_dist11(m_engine); }

private:
    std::mt19937 m_engine;
    std::uniform_real_distribution<double> m_dist01;
    std::uniform_real_distribution<double> m_dist11;
};

double clamp01(const double x)
{
    if (x < 0.0) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

Vec3 randomInUnitSphere(Random& random)
{
    while (true) {
        const Vec3 p(random.nextSigned(), random.nextSigned(), random.nextSigned());
        if (Math::getLengthSquared(p) < 1.0) {
            return p;
        }
    }
}

Vec3 randomUnitVector(Random& random)
{
    return glm::normalize(randomInUnitSphere(random));
}

Vec3 randomOnHemisphere(const Vec3& normal, Random& random)
{
    Vec3 dir = randomUnitVector(random);
    if (glm::dot(dir, normal) < 0.0) {
        dir = -dir;
    }
    return dir;
}

struct Ray {
    Vec3 origin;
    Vec3 direction;

    Vec3 at(const double t) const { return origin + direction * t; }
};

class Material;

struct HitRecord {
    Vec3 position;
    Vec3 normal;
    std::shared_ptr<Material> material;
    double t = 0.0;
    bool frontFace = true;
    double u = 0.0; // texture coordinate
    double v = 0.0; // texture coordinate

    void setFaceNormal(const Ray& ray, const Vec3& outwardNormal)
    {
        frontFace = glm::dot(ray.direction, outwardNormal) < 0.0;
        normal = frontFace ? outwardNormal : -outwardNormal;
    }
};

class Hittable {
public:
    virtual ~Hittable() = default;
    virtual bool hit(const Ray& ray, double tMin, double tMax, HitRecord& record) const = 0;
    virtual Math::Box3df getAABB() const = 0;
};

class HittableList : public Hittable {
public:
    void add(std::shared_ptr<Hittable> object)
    {
        m_objects.push_back(std::move(object));
    }

    bool hit(const Ray& ray, double tMin, double tMax, HitRecord& record) const override
    {
        HitRecord temp;
        bool hasHit = false;
        double closestSoFar = tMax;

        for (const auto& object : m_objects) {
            if (object->hit(ray, tMin, closestSoFar, temp)) {
                hasHit = true;
                closestSoFar = temp.t;
                record = temp;
            }
        }

        return hasHit;
    }

    Math::Box3df getAABB() const override
    {
        auto box = Math::Box3df::createDegeneratedBox();
        for (const auto& obj : m_objects) {
            box.add(obj->getAABB());
        }
        return box;
    }

    const std::vector<std::shared_ptr<Hittable>>& objects() const { return m_objects; }

private:
    std::vector<std::shared_ptr<Hittable>> m_objects;
};

// BVH-accelerated hittable list using Phantom::Space::BVH
class BvhHittableList : public Hittable {
public:
    explicit BvhHittableList(const std::vector<std::shared_ptr<Hittable>>& objects)
        : m_objects(objects)
    {
        m_bvhObjects.reserve(objects.size());
        m_bvhPtrs.reserve(objects.size());
        for (int i = 0; i < (int)objects.size(); ++i) {
            m_bvhObjects.push_back(
                std::make_unique<Space::BVHObject>(i, objects[i]->getAABB()));
            m_bvhPtrs.push_back(m_bvhObjects.back().get());
        }
        if (!m_bvhPtrs.empty()) {
            m_bvh = std::make_unique<Space::BVH>(m_bvhPtrs, 4);
        }
    }

    bool hit(const Ray& ray, double tMin, double tMax, HitRecord& record) const override
    {
        if (!m_bvh) return false;

        const Math::Vector3df o(
            (float)ray.origin.x, (float)ray.origin.y, (float)ray.origin.z);
        const Math::Vector3df d(
            (float)ray.direction.x, (float)ray.direction.y, (float)ray.direction.z);

        const auto candidates = m_bvh->queryRay(o, d, (float)tMin, (float)tMax);

        HitRecord temp;
        bool hasHit = false;
        double closestSoFar = tMax;
        for (const auto* bvhObj : candidates) {
            if (m_objects[bvhObj->id]->hit(ray, tMin, closestSoFar, temp)) {
                hasHit = true;
                closestSoFar = temp.t;
                record = temp;
            }
        }
        return hasHit;
    }

    Math::Box3df getAABB() const override
    {
        auto box = Math::Box3df::createDegeneratedBox();
        for (const auto& obj : m_objects) {
            box.add(obj->getAABB());
        }
        return box;
    }

private:
    std::vector<std::shared_ptr<Hittable>>              m_objects;
    std::vector<std::unique_ptr<Space::BVHObject>>      m_bvhObjects;
    std::vector<Space::BVHObject*>                      m_bvhPtrs;
    std::unique_ptr<Space::BVH>                         m_bvh;
};

class Quad : public Hittable {
public:
    Quad(const Vec3& q,
        const Vec3& u,
        const Vec3& v,
        std::shared_ptr<Material> material)
        : m_q(q), m_u(u), m_v(v), m_material(std::move(material))
    {
        m_normal = glm::normalize(glm::cross(m_u, m_v));
        m_d = glm::dot(m_normal, m_q);
        m_w = glm::cross(m_u, m_v);
        const double denom = glm::dot(m_w, m_w);
        m_w /= denom;
    }

    bool hit(const Ray& ray, double tMin, double tMax, HitRecord& record) const override
    {
        const double denom = glm::dot(m_normal, ray.direction);
        if (std::fabs(denom) < 1e-9) {
            return false;
        }

        const double t = (m_d - glm::dot(m_normal, ray.origin)) / denom;
        if (t < tMin || t > tMax) {
            return false;
        }

        const Vec3 p = ray.at(t);
        const Vec3 planarHitptVector = p - m_q;
        const double alpha = glm::dot(m_w, glm::cross(planarHitptVector, m_v));
        const double beta = glm::dot(m_w, glm::cross(m_u, planarHitptVector));

        if (alpha < 0.0 || alpha > 1.0 || beta < 0.0 || beta > 1.0) {
            return false;
        }

        record.t = t;
        record.position = p;
        record.material = m_material;
        record.setFaceNormal(ray, m_normal);
        return true;
    }

    Math::Box3df getAABB() const override
    {
        const Vec3 corners[4] = { m_q, m_q + m_u, m_q + m_v, m_q + m_u + m_v };
        auto box = Math::Box3df::createDegeneratedBox();
        for (const auto& c : corners) {
            box.add(Math::Vector3df((float)c.x, (float)c.y, (float)c.z));
        }
        // Pad to avoid zero-thickness AABB for axis-aligned quads
        constexpr float kEps = 1e-3f;
        box.add(box.getMin() - Math::Vector3df(kEps, kEps, kEps));
        box.add(box.getMax() + Math::Vector3df(kEps, kEps, kEps));
        return box;
    }

private:
    Vec3 m_q;
    Vec3 m_u;
    Vec3 m_v;
    Vec3 m_w;
    Vec3 m_normal;
    std::shared_ptr<Material> m_material;
    double m_d = 0.0;
};

class Material {
public:
    virtual ~Material() = default;

    virtual bool scatter(const Ray& rayIn, const HitRecord& record, Vec3& attenuation, Ray& scattered, Random& random) const = 0;

    virtual Vec3 emitted(double /*u*/, double /*v*/) const { return Vec3(0.0, 0.0, 0.0); }
};

class Lambertian : public Material {
public:
    explicit Lambertian(const Vec3& albedo)
        : m_albedo(albedo) {}

    bool scatter(const Ray&,
        const HitRecord& record,
        Vec3& attenuation,
        Ray& scattered,
        Random& random) const override
    {
        Vec3 direction = randomOnHemisphere(record.normal, random);
        if (Math::getLengthSquared(direction) < 1e-10) {
            direction = record.normal;
        }

        scattered = Ray{ record.position, direction };
        attenuation = m_albedo;
        return true;
    }

private:
    Vec3 m_albedo;
};

class DiffuseLight : public Material {
public:
    explicit DiffuseLight(const Vec3& emit)
        : m_emit(emit) {}

    bool scatter(const Ray&, const HitRecord&, Vec3&, Ray&, Random&) const override
    {
        return false;
    }

    Vec3 emitted(double, double) const override
    {
        return m_emit;
    }

private:
    Vec3 m_emit;
};

class Camera {
public:
    Camera(const Vec3& lookFrom,
        const Vec3& lookAt,
        const Vec3& vup,
        const double vfovDeg,
        const double aspectRatio)
        : m_origin(lookFrom)
    {
        const double theta = vfovDeg * kPi / 180.0;
        const double h = std::tan(theta / 2.0);
        const double viewportHeight = 2.0 * h;
        const double viewportWidth = aspectRatio * viewportHeight;

        const Vec3 w = glm::normalize(lookFrom - lookAt);
        const Vec3 u = glm::normalize(glm::cross(vup, w));
        const Vec3 v = glm::cross(w, u);

        m_horizontal = viewportWidth * u;
        m_vertical = viewportHeight * v;
        m_lowerLeft = m_origin - m_horizontal * 0.5 - m_vertical * 0.5 - w;
    }

    Ray getRay(const double s, const double t) const
    {
        return Ray{ m_origin, glm::normalize(m_lowerLeft + s * m_horizontal + t * m_vertical - m_origin) };
    }

private:
    Vec3 m_origin;
    Vec3 m_lowerLeft;
    Vec3 m_horizontal;
    Vec3 m_vertical;
};

// ---------------------------------------------------------------------------
// Triangle — Möller–Trumbore ray–triangle intersection
// ---------------------------------------------------------------------------
class Triangle : public Hittable {
public:
    Triangle(const Vec3& v0, const Vec3& v1, const Vec3& v2,
             std::shared_ptr<Material> mat,
             glm::dvec2 uv0 = {0.0, 0.0},
             glm::dvec2 uv1 = {0.0, 0.0},
             glm::dvec2 uv2 = {0.0, 0.0})
        : m_v0(v0), m_v1(v1), m_v2(v2), m_mat(std::move(mat))
        , m_uv0(uv0), m_uv1(uv1), m_uv2(uv2)
    {
        m_normal = glm::normalize(glm::cross(v1 - v0, v2 - v0));
    }

    bool hit(const Ray& ray, double tMin, double tMax, HitRecord& rec) const override
    {
        const Vec3   e1 = m_v1 - m_v0;
        const Vec3   e2 = m_v2 - m_v0;
        const Vec3   h  = glm::cross(ray.direction, e2);
        const double a  = glm::dot(e1, h);
        if (std::fabs(a) < 1e-9) return false;

        const double  f      = 1.0 / a;
        const Vec3    s      = ray.origin - m_v0;
        const double  u_bary = f * glm::dot(s, h);
        if (u_bary < 0.0 || u_bary > 1.0) return false;

        const Vec3   q      = glm::cross(s, e1);
        const double v_bary = f * glm::dot(ray.direction, q);
        if (v_bary < 0.0 || u_bary + v_bary > 1.0) return false;

        const double t = f * glm::dot(e2, q);
        if (t < tMin || t > tMax) return false;

        // Barycentric UV interpolation: w = 1 - u_bary - v_bary
        const double w = 1.0 - u_bary - v_bary;
        rec.u = w * m_uv0.x + u_bary * m_uv1.x + v_bary * m_uv2.x;
        rec.v = w * m_uv0.y + u_bary * m_uv1.y + v_bary * m_uv2.y;

        rec.t        = t;
        rec.position = ray.at(t);
        rec.material = m_mat;
        rec.setFaceNormal(ray, m_normal);
        return true;
    }

    Math::Box3df getAABB() const override
    {
        auto box = Math::Box3df::createDegeneratedBox();
        for (const Vec3& v : {m_v0, m_v1, m_v2})
            box.add(Math::Vector3df((float)v.x, (float)v.y, (float)v.z));
        constexpr float kEps = 1e-4f;
        box.add(box.getMin() - Math::Vector3df(kEps, kEps, kEps));
        box.add(box.getMax() + Math::Vector3df(kEps, kEps, kEps));
        return box;
    }

private:
    Vec3 m_v0, m_v1, m_v2, m_normal;
    std::shared_ptr<Material> m_mat;
    glm::dvec2 m_uv0, m_uv1, m_uv2;
};

// ---------------------------------------------------------------------------
// Sky gradient background for glTF scene rendering
// ---------------------------------------------------------------------------
static Vec3 gradientSky(const Vec3& dir)
{
    const double t = 0.5 * (glm::normalize(dir).y + 1.0);
    return Vec3(1.0) * (1.0 - t) + Vec3(0.5, 0.7, 1.0) * t;
}

static Vec3 traceGltf(const Ray& ray, const Hittable& world, int depth, Random& random)
{
    if (depth <= 0) return Vec3(0.0);

    HitRecord record;
    if (!world.hit(ray, 1e-4, kInfinity, record))
        return gradientSky(ray.direction);

    const Vec3 emitted = record.material->emitted(record.u, record.v);
    Ray  scattered;
    Vec3 attenuation;
    if (!record.material->scatter(ray, record, attenuation, scattered, random))
        return emitted;

    const Vec3 indirect = traceGltf(scattered, world, depth - 1, random);
    return emitted + Vec3(attenuation.x * indirect.x,
                          attenuation.y * indirect.y,
                          attenuation.z * indirect.z);
}

// ---------------------------------------------------------------------------
// Sphere
// ---------------------------------------------------------------------------
class Sphere : public Hittable {
public:
    Sphere(const Vec3& center, double radius, std::shared_ptr<Material> material)
        : m_center(center), m_radius(radius), m_material(std::move(material)) {}

    bool hit(const Ray& ray, double tMin, double tMax, HitRecord& record) const override
    {
        const Vec3 oc = ray.origin - m_center;
        const double a = glm::dot(ray.direction, ray.direction);
        const double halfB = glm::dot(oc, ray.direction);
        const double c = glm::dot(oc, oc) - m_radius * m_radius;
        const double discriminant = halfB * halfB - a * c;
        if (discriminant < 0.0) return false;

        const double sqrtD = std::sqrt(discriminant);
        double root = (-halfB - sqrtD) / a;
        if (root < tMin || root > tMax) {
            root = (-halfB + sqrtD) / a;
            if (root < tMin || root > tMax) return false;
        }

        record.t = root;
        record.position = ray.at(root);
        const Vec3 outwardNormal = (record.position - m_center) / m_radius;
        record.setFaceNormal(ray, outwardNormal);
        record.material = m_material;
        return true;
    }

    Math::Box3df getAABB() const override
    {
        const float r = (float)m_radius;
        const Math::Vector3df c((float)m_center.x, (float)m_center.y, (float)m_center.z);
        return Math::Box3df(c - Math::Vector3df(r, r, r), c + Math::Vector3df(r, r, r));
    }

private:
    Vec3   m_center;
    double m_radius;
    std::shared_ptr<Material> m_material;
};

// ---------------------------------------------------------------------------
// Texture system
// ---------------------------------------------------------------------------
class Texture {
public:
    virtual ~Texture() = default;
    virtual Vec3 sample(double u, double v) const = 0;
};

class SolidTexture : public Texture {
public:
    explicit SolidTexture(const Vec3& c) : m_color(c) {}
    Vec3 sample(double, double) const override { return m_color; }
private:
    Vec3 m_color;
};

// Approximate sRGB → linear (base color textures are typically sRGB-encoded)
static Vec3 srgbToLinear(const Vec3& c)
{
    return Vec3(std::pow(c.x, 2.2), std::pow(c.y, 2.2), std::pow(c.z, 2.2));
}

class ImageTexture : public Texture {
public:
    explicit ImageTexture(const RtTexture* data) : m_data(data) {}

    Vec3 sample(double u, double v) const override
    {
        if (!m_data || m_data->pixels.empty() || m_data->width <= 0 || m_data->height <= 0)
            return Vec3(1.0, 0.0, 1.0); // magenta = missing texture sentinel

        // Wrap to [0, 1)
        u = std::fmod(u, 1.0); if (u < 0.0) u += 1.0;
        v = std::fmod(v, 1.0); if (v < 0.0) v += 1.0;

        // glTF UV: v=0 is top; internal convention: v=0 is bottom → flip Y
        const double px = u * (m_data->width  - 1);
        const double py = (1.0 - v) * (m_data->height - 1);

        const int x0 = static_cast<int>(px);
        const int y0 = static_cast<int>(py);
        const int x1 = std::min(x0 + 1, m_data->width  - 1);
        const int y1 = std::min(y0 + 1, m_data->height - 1);

        const double fx = px - x0;
        const double fy = py - y0;

        auto fetch = [&](int x, int y) -> Vec3 {
            const int base = (y * m_data->width + x) * m_data->channels;
            const auto* p  = m_data->pixels.data() + base;
            return Vec3(p[0] / 255.0, p[1] / 255.0, p[2] / 255.0);
        };

        // Bilinear interpolation
        const Vec3 c0 = fetch(x0, y0) * (1.0 - fx) + fetch(x1, y0) * fx;
        const Vec3 c1 = fetch(x0, y1) * (1.0 - fx) + fetch(x1, y1) * fx;
        return c0 * (1.0 - fy) + c1 * fy;
    }

private:
    const RtTexture* m_data = nullptr; // non-owning; lifetime managed by caller
};

// ---------------------------------------------------------------------------
// PbrMaterial — Cook-Torrance metallic-roughness (simplified path tracer form)
// ---------------------------------------------------------------------------
double schlickFresnel(double cosTheta, double ior)
{
    double r0 = (1.0 - ior) / (1.0 + ior);
    r0 = r0 * r0;
    return r0 + (1.0 - r0) * std::pow(1.0 - cosTheta, 5.0);
}

Vec3 schlickFresnelVec(double cosTheta, const Vec3& f0)
{
    const double s = std::pow(1.0 - cosTheta, 5.0);
    return f0 + (Vec3(1.0) - f0) * s;
}

// Sample a direction perturbed around `mirror` by roughness (simplified GGX cone)
Vec3 sampleGGX(const Vec3& mirror, double roughness, Random& random)
{
    if (roughness < 1e-4) return mirror;
    const double alpha = roughness * roughness;
    // rejection-sample a half-vector from GGX using polar coordinates
    while (true) {
        const double xi1 = random.next01();
        const double xi2 = random.next01();
        const double cosTheta2 = (1.0 - xi1) / (1.0 + (alpha * alpha - 1.0) * xi1);
        const double cosTheta = std::sqrt(std::max(0.0, cosTheta2));
        const double sinTheta = std::sqrt(std::max(0.0, 1.0 - cosTheta2));
        const double phi = 2.0 * kPi * xi2;

        // Build local frame around mirror direction
        Vec3 up = std::fabs(mirror.y) < 0.999 ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
        const Vec3 tangent = glm::normalize(glm::cross(up, mirror));
        const Vec3 bitangent = glm::cross(mirror, tangent);

        const Vec3 h = glm::normalize(
            sinTheta * std::cos(phi) * tangent +
            sinTheta * std::sin(phi) * bitangent +
            cosTheta * mirror);

        const Vec3 scattered = glm::reflect(-mirror, h);
        if (glm::dot(scattered, mirror) >= 0.0) return glm::normalize(scattered);
    }
}

class PbrMaterial : public Material {
public:
    PbrMaterial(
        const Vec3& albedo,
        double metallic,
        double roughness,
        const Vec3& emission,
        double transmission,
        double ior,
        std::shared_ptr<Texture> baseColorTex  = nullptr,
        std::shared_ptr<Texture> metalRoughTex = nullptr,
        std::shared_ptr<Texture> emissiveTex   = nullptr)
        : m_albedo(albedo)
        , m_metallic(metallic)
        , m_roughness(roughness)
        , m_emission(emission)
        , m_transmission(transmission)
        , m_ior(ior)
        , m_baseColorTex(std::move(baseColorTex))
        , m_metalRoughTex(std::move(metalRoughTex))
        , m_emissiveTex(std::move(emissiveTex))
    {}

    Vec3 emitted(double u, double v) const override
    {
        if (m_emissiveTex)
            return srgbToLinear(m_emissiveTex->sample(u, v)) * m_emission;
        return m_emission;
    }

    bool scatter(const Ray& rayIn,
        const HitRecord& record,
        Vec3& attenuation,
        Ray& scattered,
        Random& random) const override
    {
        // Resolve texture-overridden material properties
        const Vec3 effectiveAlbedo = m_baseColorTex
            ? srgbToLinear(m_baseColorTex->sample(record.u, record.v))
            : m_albedo;

        double effectiveRoughness = m_roughness;
        double effectiveMetallic  = m_metallic;
        if (m_metalRoughTex) {
            const Vec3 mr    = m_metalRoughTex->sample(record.u, record.v); // linear
            effectiveRoughness = mr.y; // G channel
            effectiveMetallic  = mr.z; // B channel
        }

        const Vec3 n = record.normal;
        const Vec3 wo = -glm::normalize(rayIn.direction);
        const double cosTheta = std::max(0.0, glm::dot(wo, n));

        // Schlick Fresnel F0: metal=albedo, dielectric=0.04
        const Vec3 f0 = Vec3(0.04) * (1.0 - effectiveMetallic) + effectiveAlbedo * effectiveMetallic;
        const Vec3 F = schlickFresnelVec(cosTheta, f0);
        const double fLum = 0.2126 * F.x + 0.7152 * F.y + 0.0722 * F.z;

        const double pReflect  = fLum;
        const double pTransmit = m_transmission * (1.0 - fLum);

        const double rng = random.next01();

        if (rng < pReflect) {
            // --- Specular reflection (GGX) ---
            const Vec3 mirror = glm::reflect(-wo, n);
            const Vec3 dir = sampleGGX(mirror, effectiveRoughness, random);
            if (glm::dot(dir, n) <= 0.0) return false;
            scattered = Ray{ record.position, dir };
            attenuation = F / pReflect;
            return true;
        }
        else if (rng < pReflect + pTransmit) {
            // --- Transmission (refraction) ---
            const double ratio = record.frontFace ? (1.0 / m_ior) : m_ior;
            const double cos2 = glm::dot(-glm::normalize(rayIn.direction), n);
            const double schlick = schlickFresnel(cos2, m_ior);
            if (random.next01() < schlick) {
                // Total internal reflection fallback
                const Vec3 dir = glm::reflect(glm::normalize(rayIn.direction), n);
                scattered = Ray{ record.position, dir };
                attenuation = effectiveAlbedo;
                return true;
            }
            const Vec3 refracted = glm::refract(glm::normalize(rayIn.direction), n, ratio);
            scattered = Ray{ record.position, refracted };
            attenuation = effectiveAlbedo / pTransmit;
            return true;
        }
        else {
            // --- Diffuse (Lambertian) — only for non-metallic ---
            if (effectiveMetallic >= 1.0 - 1e-4) return false;
            const Vec3 dir = randomOnHemisphere(n, random);
            scattered = Ray{ record.position, dir };
            const double pDiffuse = 1.0 - pReflect - pTransmit;
            if (pDiffuse < 1e-6) return false;
            attenuation = effectiveAlbedo * (1.0 - effectiveMetallic) * (Vec3(1.0) - F) / pDiffuse;
            return true;
        }
    }

private:
    Vec3   m_albedo;
    double m_metallic     = 0.0;
    double m_roughness    = 0.5;
    Vec3   m_emission     = Vec3(0.0);
    double m_transmission = 0.0;
    double m_ior          = 1.5;
    std::shared_ptr<Texture> m_baseColorTex;
    std::shared_ptr<Texture> m_metalRoughTex;
    std::shared_ptr<Texture> m_emissiveTex;
};

// ---------------------------------------------------------------------------
// SceneLoader — .cscene JSON → SceneData
// ---------------------------------------------------------------------------
// Forward declaration (defined later in this file)
void addAxisAlignedBox(HittableList& scene, const Vec3& a, const Vec3& b,
    const std::shared_ptr<Material>& material);

using Json = nlohmann::json;

struct SceneData {
    HittableList          world;
    std::optional<Camera> camera;
    RenderSettings        settings;
    std::string           outputPath = "output.png";
    Vec3                  background = Vec3(0.0);
};

static Vec3 toVec3(const Json& j)
{
    return Vec3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>());
}

static std::optional<SceneData> loadScene(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) return std::nullopt;

    const Json root = Json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) return std::nullopt;

    SceneData data;

    // --- render settings ---
    if (root.contains("render")) {
        const auto& r = root["render"];
        if (r.contains("width"))            data.settings.width           = r["width"];
        if (r.contains("height"))           data.settings.height          = r["height"];
        if (r.contains("samples_per_pixel"))data.settings.samplesPerPixel = r["samples_per_pixel"];
        if (r.contains("max_depth"))        data.settings.maxDepth        = r["max_depth"];
        if (r.contains("seed"))             data.settings.randomSeed      = r["seed"].get<std::uint32_t>();
        if (r.contains("output"))           data.outputPath               = r["output"].get<std::string>();
    }

    // --- background ---
    if (root.contains("background")) {
        data.background = toVec3(root["background"]);
    }

    // --- materials ---
    std::map<std::string, std::shared_ptr<Material>> matMap;
    if (root.contains("materials")) {
        for (const auto& m : root["materials"]) {
            const std::string name = m["name"];
            const std::string type = m["type"];
            std::shared_ptr<Material> mat;

            if (type == "lambertian") {
                mat = std::make_shared<Lambertian>(toVec3(m["albedo"]));
            }
            else if (type == "diffuse_light") {
                mat = std::make_shared<DiffuseLight>(toVec3(m["emit"]));
            }
            else if (type == "pbr") {
                const Vec3   albedo       = m.contains("albedo")       ? toVec3(m["albedo"])       : Vec3(0.8);
                const double metallic     = m.contains("metallic")     ? m["metallic"].get<double>()     : 0.0;
                const double roughness    = m.contains("roughness")    ? m["roughness"].get<double>()    : 0.5;
                const Vec3   emission     = m.contains("emission")     ? toVec3(m["emission"])     : Vec3(0.0);
                const double transmission = m.contains("transmission") ? m["transmission"].get<double>() : 0.0;
                const double ior          = m.contains("ior")          ? m["ior"].get<double>()          : 1.5;
                mat = std::make_shared<PbrMaterial>(albedo, metallic, roughness, emission, transmission, ior);
            }
            else {
                continue; // unknown type — skip
            }
            matMap[name] = std::move(mat);
        }
    }

    auto getMat = [&](const std::string& name) -> std::shared_ptr<Material> {
        auto it = matMap.find(name);
        if (it != matMap.end()) return it->second;
        return std::make_shared<Lambertian>(Vec3(0.8, 0.0, 0.8)); // magenta = missing
    };

    // --- objects ---
    if (root.contains("objects")) {
        for (const auto& obj : root["objects"]) {
            const std::string type    = obj["type"];
            const std::string matName = obj.contains("material") ? obj["material"].get<std::string>() : "";
            auto mat = getMat(matName);

            if (type == "quad") {
                data.world.add(std::make_shared<Quad>(
                    toVec3(obj["q"]), toVec3(obj["u"]), toVec3(obj["v"]), mat));
            }
            else if (type == "sphere") {
                data.world.add(std::make_shared<Sphere>(
                    toVec3(obj["center"]), obj["radius"].get<double>(), mat));
            }
            else if (type == "box") {
                addAxisAlignedBox(data.world, toVec3(obj["min"]), toVec3(obj["max"]), mat);
            }
        }
    }

    // --- camera ---
    {
        Vec3   lookFrom(278, 278, -800);
        Vec3   lookAt(278, 278, 0);
        Vec3   up(0, 1, 0);
        double fov = 40.0;

        if (root.contains("camera")) {
            const auto& c = root["camera"];
            if (c.contains("look_from")) lookFrom = toVec3(c["look_from"]);
            if (c.contains("look_at"))   lookAt   = toVec3(c["look_at"]);
            if (c.contains("up"))        up       = toVec3(c["up"]);
            if (c.contains("fov"))       fov      = c["fov"].get<double>();
        }

        const double aspect = static_cast<double>(data.settings.width) /
                              static_cast<double>(data.settings.height);
        data.camera.emplace(lookFrom, lookAt, up, fov, aspect);
    }

    return data;
}

void addAxisAlignedBox(HittableList& scene,
    const Vec3& a,
    const Vec3& b,
    const std::shared_ptr<Material>& material)
{
    const Vec3 min(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z));
    const Vec3 max(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z));

    const Vec3 dx(max.x - min.x, 0.0, 0.0);
    const Vec3 dy(0.0, max.y - min.y, 0.0);
    const Vec3 dz(0.0, 0.0, max.z - min.z);

    scene.add(std::make_shared<Quad>(Vec3(min.x, min.y, max.z), dx, dy, material));
    scene.add(std::make_shared<Quad>(Vec3(max.x, min.y, min.z), -dz, dy, material));
    scene.add(std::make_shared<Quad>(Vec3(max.x, min.y, max.z), -dx, dy, material));
    scene.add(std::make_shared<Quad>(Vec3(min.x, min.y, min.z), dz, dy, material));
    scene.add(std::make_shared<Quad>(Vec3(min.x, max.y, max.z), dx, -dz, material));
    scene.add(std::make_shared<Quad>(Vec3(min.x, min.y, min.z), dx, dz, material));
}

Vec3 trace(const Ray& ray, const Hittable& world, int depth, Random& random,
    const Vec3& background = Vec3(0.0))
{
    if (depth <= 0) {
        return Vec3(0.0, 0.0, 0.0);
    }

    HitRecord record;
    if (!world.hit(ray, 1e-4, kInfinity, record)) {
        return background;
    }

    const Vec3 emitted = record.material->emitted(record.u, record.v);

    Ray scattered;
    Vec3 attenuation;
    if (!record.material->scatter(ray, record, attenuation, scattered, random)) {
        return emitted;
    }

    const Vec3 indirect = trace(scattered, world, depth - 1, random, background);
    return emitted + Vec3(attenuation.x * indirect.x, attenuation.y * indirect.y, attenuation.z * indirect.z);
}

HittableList buildCornellBox()
{
    HittableList scene;

    const auto red = std::make_shared<Lambertian>(Vec3(0.65, 0.05, 0.05));
    const auto white = std::make_shared<Lambertian>(Vec3(0.73, 0.73, 0.73));
    const auto green = std::make_shared<Lambertian>(Vec3(0.12, 0.45, 0.15));
    const auto light = std::make_shared<DiffuseLight>(Vec3(15.0, 15.0, 15.0));

    scene.add(std::make_shared<Quad>(Vec3(555.0, 0.0, 0.0), Vec3(0.0, 555.0, 0.0), Vec3(0.0, 0.0, 555.0), green));
    scene.add(std::make_shared<Quad>(Vec3(0.0, 0.0, 555.0), Vec3(0.0, 555.0, 0.0), Vec3(0.0, 0.0, -555.0), red));

    scene.add(std::make_shared<Quad>(Vec3(343.0, 554.0, 332.0), Vec3(-130.0, 0.0, 0.0), Vec3(0.0, 0.0, -105.0), light));

    scene.add(std::make_shared<Quad>(Vec3(0.0, 0.0, 0.0), Vec3(555.0, 0.0, 0.0), Vec3(0.0, 0.0, 555.0), white));
    scene.add(std::make_shared<Quad>(Vec3(555.0, 555.0, 555.0), Vec3(-555.0, 0.0, 0.0), Vec3(0.0, 0.0, -555.0), white));
    scene.add(std::make_shared<Quad>(Vec3(0.0, 0.0, 555.0), Vec3(555.0, 0.0, 0.0), Vec3(0.0, 555.0, 0.0), white));

    addAxisAlignedBox(scene, Vec3(130.0, 0.0, 65.0), Vec3(295.0, 165.0, 230.0), white);
    addAxisAlignedBox(scene, Vec3(265.0, 0.0, 295.0), Vec3(430.0, 330.0, 460.0), white);

    return scene;
}

Graphics::ColorRGBAuc toRGBA(const Vec3& color, const int samplesPerPixel)
{
    const double scale = 1.0 / static_cast<double>(samplesPerPixel);
    const double r = std::sqrt(clamp01(color.x * scale));
    const double g = std::sqrt(clamp01(color.y * scale));
    const double b = std::sqrt(clamp01(color.z * scale));

    return Graphics::ColorRGBAuc(
        static_cast<unsigned char>(255.999 * r),
        static_cast<unsigned char>(255.999 * g),
        static_cast<unsigned char>(255.999 * b),
        static_cast<unsigned char>(255));
}

// Distributes rows of the image across worker threads. 'traceRay' is invoked as
// traceRay(ray, random) -> Vec3 for a single sample; world/camera/textures it closes
// over are read-only for the duration of the render, and each thread owns a private
// Random stream, so concurrent output.setColor() calls (disjoint pixels, pre-sized
// buffer) are safe without further synchronization.
template <typename TraceRayFn>
void renderRowsParallel(Graphics::Imageuc& output, const Camera& camera,
                         int width, int height, int spp, std::uint32_t baseSeed,
                         TraceRayFn&& traceRay)
{
    output = Graphics::Imageuc(width, height);

    unsigned int numThreads = std::thread::hardware_concurrency();
    numThreads = std::max(1u, std::min(numThreads == 0 ? 4u : numThreads,
                                        static_cast<unsigned int>(height)));

    std::atomic<int> nextRow{0};
    std::atomic<int> rowsDone{0};
    std::mutex progressMutex;
    int nextPercent = 10;

    std::vector<std::thread> pool;
    pool.reserve(numThreads);
    for (unsigned int t = 0; t < numThreads; ++t) {
        pool.emplace_back([&, t]() {
            Random random(baseSeed + static_cast<std::uint32_t>(t) * 0x9E3779B1u + 1u);
            int y;
            while ((y = nextRow.fetch_add(1)) < height) {
                for (int x = 0; x < width; ++x) {
                    Vec3 color(0.0);
                    for (int s = 0; s < spp; ++s) {
                        const double u = (static_cast<double>(x) + random.next01()) / static_cast<double>(width  - 1);
                        const double v = (static_cast<double>(y) + random.next01()) / static_cast<double>(height - 1);
                        color += traceRay(camera.getRay(u, 1.0 - v), random);
                    }
                    output.setColor(x, y, toRGBA(color, spp));
                }
                const int done = ++rowsDone;
                std::lock_guard<std::mutex> lock(progressMutex);
                reportProgress(done - 1, height, nextPercent);
            }
        });
    }
    for (auto& th : pool) th.join();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Legacy free function implementations
// ---------------------------------------------------------------------------

bool renderCornellBox(const RenderSettings& settings, Graphics::Imageuc& output)
{
    if (settings.width <= 0 || settings.height <= 0 || settings.samplesPerPixel <= 0 || settings.maxDepth <= 0) {
        return false;
    }

    const auto start = std::chrono::steady_clock::now();

    std::cout << "Rendering Cornell Box: "
              << settings.width << "x" << settings.height
              << ", spp=" << settings.samplesPerPixel
              << ", depth=" << settings.maxDepth << std::endl;

    const HittableList flatWorld = buildCornellBox();
    const BvhHittableList world(flatWorld.objects());
    const Camera camera(
        Vec3(278.0, 278.0, -800.0),
        Vec3(278.0, 278.0, 0.0),
        Vec3(0.0, 1.0, 0.0),
        40.0,
        static_cast<double>(settings.width) / static_cast<double>(settings.height));

    renderRowsParallel(output, camera, settings.width, settings.height,
                        settings.samplesPerPixel, settings.randomSeed,
                        [&](const Ray& ray, Random& random) {
                            return trace(ray, world, settings.maxDepth, random);
                        });

    const auto end = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "Render complete in " << (elapsedMs / 1000.0) << " sec" << std::endl;

    return true;
}

SceneRenderResult renderScene(const std::string& scenePath, Graphics::Imageuc& output)
{
    auto data = loadScene(scenePath);
    if (!data) {
        return { false, "", "Failed to load scene: " + scenePath };
    }

    const RenderSettings& s = data->settings;
    if (s.width <= 0 || s.height <= 0 || s.samplesPerPixel <= 0 || s.maxDepth <= 0) {
        return { false, "", "Invalid render settings in scene file." };
    }

    const auto start = std::chrono::steady_clock::now();

    std::cout << "Rendering scene: "
              << s.width << "x" << s.height
              << ", spp=" << s.samplesPerPixel
              << ", depth=" << s.maxDepth << std::endl;

    const BvhHittableList world(data->world.objects());

    renderRowsParallel(output, *data->camera, s.width, s.height,
                        s.samplesPerPixel, s.randomSeed,
                        [&](const Ray& ray, Random& random) {
                            return trace(ray, world, s.maxDepth, random, data->background);
                        });

    const auto end = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "Render complete in " << (elapsedMs / 1000.0) << " sec" << std::endl;

    return { true, data->outputPath, "" };
}

bool renderCornellBoxWithCamera(const RenderSettings& settings,
                                const RtCameraSpec& cam,
                                Graphics::Imageuc& output)
{
    if (settings.width <= 0 || settings.height <= 0 ||
        settings.samplesPerPixel <= 0 || settings.maxDepth <= 0)
        return false;

    const HittableList flatWorld = buildCornellBox();
    const BvhHittableList world(flatWorld.objects());
    const Camera camera(
        Vec3(cam.lookFrom[0], cam.lookFrom[1], cam.lookFrom[2]),
        Vec3(cam.lookAt[0],   cam.lookAt[1],   cam.lookAt[2]),
        Vec3(cam.up[0],       cam.up[1],        cam.up[2]),
        cam.fovDeg,
        static_cast<double>(settings.width) / static_cast<double>(settings.height));

    renderRowsParallel(output, camera, settings.width, settings.height,
                        settings.samplesPerPixel, settings.randomSeed,
                        [&](const Ray& ray, Random& random) {
                            return trace(ray, world, settings.maxDepth, random);
                        });
    return true;
}

static bool renderTriangleSceneWithTextures(
    const std::vector<RtTriangle>& triangles,
    const RtCameraSpec&            cam,
    const RenderSettings&          settings,
    Graphics::Imageuc&             output,
    const std::vector<RtTexture>&  textures)
{
    if (settings.width <= 0 || settings.height <= 0 ||
        settings.samplesPerPixel <= 0 || settings.maxDepth <= 0)
        return false;

    // Wrap each RtTexture in an ImageTexture — non-owning pointers; 'textures' must outlive this call
    std::vector<std::shared_ptr<Texture>> texObjs;
    texObjs.reserve(textures.size());
    for (const auto& t : textures)
        texObjs.push_back(std::make_shared<ImageTexture>(&t));

    auto getTex = [&](int idx) -> std::shared_ptr<Texture> {
        if (idx < 0 || idx >= static_cast<int>(texObjs.size())) return nullptr;
        return texObjs[idx];
    };

    const auto start = std::chrono::steady_clock::now();
    std::cout << "Rendering glTF scene (" << triangles.size() << " triangles, "
              << textures.size() << " textures): "
              << settings.width << "x" << settings.height
              << ", spp=" << settings.samplesPerPixel
              << ", depth=" << settings.maxDepth << std::endl;

    HittableList flat;
    for (const auto& tri : triangles) {
        auto mat = std::make_shared<PbrMaterial>(
            Vec3(tri.albedo[0],   tri.albedo[1],   tri.albedo[2]),
            tri.metallic,
            tri.roughness,
            Vec3(tri.emission[0], tri.emission[1], tri.emission[2]),
            0.0, 1.5,
            getTex(tri.baseColorTextureIndex),
            getTex(tri.metallicRoughnessTextureIndex),
            getTex(tri.emissiveTextureIndex));
        flat.add(std::make_shared<Triangle>(
            Vec3(tri.v0[0], tri.v0[1], tri.v0[2]),
            Vec3(tri.v1[0], tri.v1[1], tri.v1[2]),
            Vec3(tri.v2[0], tri.v2[1], tri.v2[2]),
            std::move(mat),
            glm::dvec2(tri.uv0[0], tri.uv0[1]),
            glm::dvec2(tri.uv1[0], tri.uv1[1]),
            glm::dvec2(tri.uv2[0], tri.uv2[1])));
    }

    const BvhHittableList world(flat.objects());
    const Camera camera(
        Vec3(cam.lookFrom[0], cam.lookFrom[1], cam.lookFrom[2]),
        Vec3(cam.lookAt[0],   cam.lookAt[1],   cam.lookAt[2]),
        Vec3(cam.up[0],       cam.up[1],        cam.up[2]),
        cam.fovDeg,
        static_cast<double>(settings.width) / settings.height);

    renderRowsParallel(output, camera, settings.width, settings.height,
                        settings.samplesPerPixel, settings.randomSeed,
                        [&](const Ray& ray, Random& random) {
                            return traceGltf(ray, world, settings.maxDepth, random);
                        });

    const auto end = std::chrono::steady_clock::now();
    std::cout << "Render complete in "
              << (std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() / 1000.0)
              << " sec" << std::endl;
    return true;
}

bool renderTriangleScene(const std::vector<RtTriangle>& triangles,
                         const RtCameraSpec&            cam,
                         const RenderSettings&          settings,
                         Graphics::Imageuc&             output)
{
    if (settings.width <= 0 || settings.height <= 0 ||
        settings.samplesPerPixel <= 0 || settings.maxDepth <= 0)
        return false;

    const auto start = std::chrono::steady_clock::now();
    std::cout << "Rendering glTF scene (" << triangles.size() << " triangles): "
              << settings.width << "x" << settings.height
              << ", spp=" << settings.samplesPerPixel
              << ", depth=" << settings.maxDepth << std::endl;

    HittableList flat;
    for (const auto& tri : triangles) {
        auto mat = std::make_shared<PbrMaterial>(
            Vec3(tri.albedo[0],   tri.albedo[1],   tri.albedo[2]),
            tri.metallic,
            tri.roughness,
            Vec3(tri.emission[0], tri.emission[1], tri.emission[2]),
            0.0, 1.5);
        flat.add(std::make_shared<Triangle>(
            Vec3(tri.v0[0], tri.v0[1], tri.v0[2]),
            Vec3(tri.v1[0], tri.v1[1], tri.v1[2]),
            Vec3(tri.v2[0], tri.v2[1], tri.v2[2]),
            std::move(mat),
            glm::dvec2(tri.uv0[0], tri.uv0[1]),
            glm::dvec2(tri.uv1[0], tri.uv1[1]),
            glm::dvec2(tri.uv2[0], tri.uv2[1])));
    }

    const BvhHittableList world(flat.objects());
    const Camera camera(
        Vec3(cam.lookFrom[0], cam.lookFrom[1], cam.lookFrom[2]),
        Vec3(cam.lookAt[0],   cam.lookAt[1],   cam.lookAt[2]),
        Vec3(cam.up[0],       cam.up[1],        cam.up[2]),
        cam.fovDeg,
        static_cast<double>(settings.width) / settings.height);

    renderRowsParallel(output, camera, settings.width, settings.height,
                        settings.samplesPerPixel, settings.randomSeed,
                        [&](const Ray& ray, Random& random) {
                            return traceGltf(ray, world, settings.maxDepth, random);
                        });

    const auto end = std::chrono::steady_clock::now();
    std::cout << "Render complete in "
              << (std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() / 1000.0)
              << " sec" << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// PathTracer class method implementations
// Suppress C4996: class methods intentionally delegate to the free functions
// in the same translation unit — not external deprecated API.
// ---------------------------------------------------------------------------
#pragma warning(push)
#pragma warning(disable: 4996)

PathTracer::PathTracer(const RenderSettings& settings)
    : settings_(settings) {}

void PathTracer::setSettings(const RenderSettings& settings)
{
    settings_ = settings;
}

const RenderSettings& PathTracer::getSettings() const
{
    return settings_;
}

bool PathTracer::render(Graphics::Imageuc& output) const
{
    return renderCornellBox(settings_, output);
}

bool PathTracer::render(const RtCameraSpec& cam, Graphics::Imageuc& output) const
{
    return renderCornellBoxWithCamera(settings_, cam, output);
}

bool PathTracer::render(const std::vector<RtTriangle>& triangles,
                        const RtCameraSpec& cam,
                        Graphics::Imageuc& output) const
{
    return renderTriangleScene(triangles, cam, settings_, output);
}

bool PathTracer::render(const std::vector<RtTriangle>& triangles,
                        const RtCameraSpec& cam,
                        Graphics::Imageuc& output,
                        const std::vector<RtTexture>& textures) const
{
    return renderTriangleSceneWithTextures(triangles, cam, settings_, output, textures);
}

SceneRenderResult PathTracer::renderScene(const std::string& scenePath,
                                          Graphics::Imageuc& output) const
{
    // Use fully qualified name to call the free function, not the member function
    return ::Phantom::RayTracer::renderScene(scenePath, output);
}

#pragma warning(pop)

} // namespace Phantom::RayTracer
