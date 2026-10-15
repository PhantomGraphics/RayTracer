#pragma once
// Phantom::PBR::Light -> RtLight (header-only; the path tracer's explicit light sources, PathTracer.h).
//
// Intensity: the tracer's direct light is albedo / pi * intensity, the viewport's is albedo * radiance, so the tracer gets
// intensity * pi to match. An area light is a one-sided rectangle (halfU / halfV are its half-width / half-height axes).
#include "PathTracer.h"
#include "../../CGLib/PBR/PBR/Light.h"

namespace Phantom::RayTracer {

	inline RtLight toRtLight(const Phantom::PBR::Light& l) {
		constexpr double kPi = 3.14159265358979323846;
		RtLight r;
		r.intensity = static_cast<double>(l.intensity) * kPi;
		for (int i = 0; i < 3; ++i) {
			r.color[i] = l.color[i];
			r.position[i] = l.position[i];
			r.direction[i] = l.direction[i];
		}
		switch (l.type) {
		case Phantom::PBR::LightType::Directional:
			r.type = RtLight::Type::Directional;
			break;
		case Phantom::PBR::LightType::Point:
			r.type = RtLight::Type::Point;
			break;
		case Phantom::PBR::LightType::Spot:
			r.type = RtLight::Type::Spot;
			r.outerAngle = l.spotAngleRad;
			r.innerAngle = l.spotAngleRad * (1.0 - l.spotBlend);
			break;
		case Phantom::PBR::LightType::Area: {
			r.type = RtLight::Type::Area;
			const glm::vec3 up = glm::normalize(l.up);
			const glm::vec3 right = glm::normalize(glm::cross(l.direction, up));
			for (int i = 0; i < 3; ++i) {
				r.halfU[i] = right[i] * 0.5 * l.areaWidth;
				r.halfV[i] = up[i] * 0.5 * l.areaHeight;
			}
			break;
		}
		}
		return r;
	}

}
