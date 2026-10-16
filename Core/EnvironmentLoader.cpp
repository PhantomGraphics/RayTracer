#include "EnvironmentLoader.h"

#include "../../CGLib/Graphics/Core/ImageFileReader.h"

#include <algorithm>
#include <filesystem>
#include <mutex>

namespace Phantom::RayTracer {

namespace {
	constexpr int kMaxWidth = 1024;

	struct Cached {
		std::string path;
		std::filesystem::file_time_type stamp{};
		std::shared_ptr<const RtEnvironment> image;
	};

	// UTF-8 std::string -> path (a plain std::string would be read in the ANSI code page on Windows).
	std::filesystem::path utf8Path(const std::string& utf8) {
		return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
	}
}

std::shared_ptr<const RtEnvironment> loadEnvironment(const std::string& path, float intensity, bool background) {
	if (path.empty()) return nullptr;
	static std::mutex mutex;
	static Cached cache;
	std::lock_guard<std::mutex> lock(mutex);

	std::error_code ec;
	const auto stamp = std::filesystem::last_write_time(utf8Path(path), ec);
	if (ec) return nullptr;
	if (cache.path != path || cache.stamp != stamp || !cache.image) {
		Graphics::HDRImageFileReader reader;
		if (!reader.read(path)) return nullptr;
		const Graphics::Imagef source = reader.toImage();
		const int w = source.getWidth(), h = source.getHeight();
		if (w <= 0 || h <= 0) return nullptr;
		const std::vector<float> px = source.getValues();   // RGBA
		const int factor = std::max(1, (w + kMaxWidth - 1) / kMaxWidth);
		auto env = std::make_shared<RtEnvironment>();
		env->width = std::max(1, w / factor);
		env->height = std::max(1, h / factor);
		env->rgb.assign(static_cast<size_t>(env->width) * env->height * 3, 0.f);
		for (int y = 0; y < env->height; ++y) {
			for (int x = 0; x < env->width; ++x) {
				double sum[3] = { 0, 0, 0 };
				int n = 0;
				for (int dy = 0; dy < factor; ++dy) {
					for (int dx = 0; dx < factor; ++dx) {
						const int sx = std::min(w - 1, x * factor + dx), sy = std::min(h - 1, y * factor + dy);
						const float* p = &px[(static_cast<size_t>(sy) * w + sx) * 4];
						sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2];
						++n;
					}
				}
				float* out = &env->rgb[(static_cast<size_t>(y) * env->width + x) * 3];
				for (int c = 0; c < 3; ++c) out[c] = static_cast<float>(sum[c] / n);
			}
		}
		cache.path = path;
		cache.stamp = stamp;
		cache.image = env;
	}
	auto result = std::make_shared<RtEnvironment>(*cache.image);
	result->intensity = intensity;
	result->background = background;
	return result;
}

} // namespace Phantom::RayTracer
