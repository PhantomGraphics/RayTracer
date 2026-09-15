#include "pch.h"
#include "CommandDispatcher.h"
#include "RayTracerApp.h"

#include "../../CGLib/Graphics/ImageFileReader.h"

#include <charconv>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace Phantom::Gltf;

namespace {

bool tryInt(const std::string& s, int& out) {
    const char* b = s.data();
    const char* e = b + s.size();
    const auto [ptr, ec] = std::from_chars(b, e, out);
    return ec == std::errc{} && ptr == e;
}

bool tryFloat(const std::string& s, float& out) {
    const char* b = s.data();
    const char* e = b + s.size();
    const auto [ptr, ec] = std::from_chars(b, e, out);
    return ec == std::errc{} && ptr == e;
}

// Splits "w,h,spp,depth" into its 4 comma-separated fields.
std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',') {
            parts.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

// Phase 4B item 5 "report the raster/offline diff": mean and max per-channel absolute
// difference (RGB only, 0-255 scale) between two equally-sized PNGs -- typically a raster
// Screenshot: and a SaveRayTraceResult: of the same camera/scene. Formats a "Diff:..." response
// directly since this has no async dependency (both files are already on disk by the time a
// scenario issues this command).
std::string compareImages(const std::string& pathA, const std::string& pathB) {
    Phantom::Graphics::ImageFileReader readerA, readerB;
    if (!readerA.read(pathA)) return "Error:cannot read " + pathA;
    if (!readerB.read(pathB)) return "Error:cannot read " + pathB;

    const auto imgA = readerA.toImage();
    const auto imgB = readerB.toImage();
    if (imgA.getWidth() != imgB.getWidth() || imgA.getHeight() != imgB.getHeight()) {
        return "Error:size mismatch " +
               std::to_string(imgA.getWidth()) + "x" + std::to_string(imgA.getHeight()) + " vs " +
               std::to_string(imgB.getWidth()) + "x" + std::to_string(imgB.getHeight());
    }

    const int w = imgA.getWidth();
    const int h = imgA.getHeight();
    double sumDiff = 0.0;
    int    maxDiff = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto ca = imgA.getColor(x, y);
            const auto cb = imgB.getColor(x, y);
            for (int c = 0; c < 3; ++c) {
                const int diff = std::abs(static_cast<int>(ca[c]) - static_cast<int>(cb[c]));
                sumDiff += diff;
                maxDiff = std::max(maxDiff, diff);
            }
        }
    }
    const double mean = sumDiff / (static_cast<double>(w) * h * 3.0);

    char buf[128];
    std::snprintf(buf, sizeof(buf), "Diff:mean=%.6f,max=%d,width=%d,height=%d", mean, maxDiff, w, h);
    return buf;
}

} // namespace

void CommandDispatcher::dispatch(const std::string& command) {
    std::lock_guard<std::mutex> lock(mutex_);
    inputQueue_.push(command);
}

std::vector<std::string> CommandDispatcher::collectResponses() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    while (!outputQueue_.empty()) {
        out.push_back(std::move(outputQueue_.front()));
        outputQueue_.pop();
    }
    return out;
}

void CommandDispatcher::processQueue() {
    std::queue<std::string> local;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::swap(local, inputQueue_);
    }

    while (!local.empty()) {
        std::string cmd = std::move(local.front());
        local.pop();
        std::string resp = route(cmd);
        if (!resp.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            outputQueue_.push(std::move(resp));
        }
    }
}

std::optional<std::filesystem::path> CommandDispatcher::takePendingLoad() {
    std::optional<std::filesystem::path> p;
    p.swap(pendingLoad_);
    return p;
}

void CommandDispatcher::signalLoaded(bool ok, const std::string& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ok) {
        outputQueue_.push("OK:" + (msg.empty() ? std::string("loaded") : msg));
    } else {
        outputQueue_.push("Error:" + msg);
    }
}

std::optional<std::filesystem::path> CommandDispatcher::takePendingScreenshot() {
    std::optional<std::filesystem::path> p;
    p.swap(pendingScreenshot_);
    return p;
}

void CommandDispatcher::signalScreenshotDone(bool ok, const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    outputQueue_.push(ok ? "OK:saved " + path : "Error:screenshot failed");
}

std::optional<CommandDispatcher::RayTraceRequest> CommandDispatcher::takePendingRayTrace() {
    std::optional<RayTraceRequest> r;
    r.swap(pendingRayTrace_);
    return r;
}

void CommandDispatcher::signalRayTraceDone(bool ok, int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ok) {
        outputQueue_.push("OK:" + std::to_string(width) + "x" + std::to_string(height));
    } else {
        outputQueue_.push("Error:ray trace failed");
    }
}

std::string CommandDispatcher::route(const std::string& cmd) {
    if (cmd == "GetStatus") {
        return "OK";
    }

    if (cmd == "GetMeshCount") {
        if (!doc_) return "MeshCount:0";
        return "MeshCount:" + std::to_string(doc_->meshes.size());
    }

    if (cmd == "GetMaterialCount") {
        if (!doc_) return "MaterialCount:0";
        return "MaterialCount:" + std::to_string(doc_->materials.size());
    }

    if (cmd == "GetTextureCount") {
        if (!doc_) return "TextureCount:0";
        return "TextureCount:" + std::to_string(doc_->textures.size());
    }

    if (cmd == "GetCamDist") {
        if (!renderer_) return "Val:0";
        return "Val:" + std::to_string(static_cast<int>(*renderer_->camDistPtr()));
    }

    if (cmd.rfind("SetCamDist:", 0) == 0) {
        if (!renderer_) return "Error:no renderer";
        float v;
        if (!tryFloat(cmd.substr(11), v)) return "Error:invalid float";
        *renderer_->camDistPtr() = v;
        return "OK";
    }

    if (cmd == "ResetCamera") {
        if (!renderer_) return "Error:no renderer";
        *renderer_->camDistPtr()   = 3.0f;
        *renderer_->camTargetPtr() = {0.f, 0.f, 0.f};
        return "OK";
    }

    if (cmd.rfind("LoadFile:", 0) == 0) {
        pendingLoad_ = std::filesystem::path(cmd.substr(9));
        return {};
    }

    if (cmd.rfind("SaveScreenshot:", 0) == 0) {
        pendingScreenshot_ = std::filesystem::path(cmd.substr(15));
        return {};
    }

    if (cmd.rfind("RunRayTrace:", 0) == 0) {
        const auto parts = splitCsv(cmd.substr(12));
        if (parts.size() != 4) return "Error:RunRayTrace requires w,h,spp,depth";
        RayTraceRequest req;
        if (!tryInt(parts[0], req.width)  || !tryInt(parts[1], req.height) ||
            !tryInt(parts[2], req.spp)    || !tryInt(parts[3], req.depth))
            return "Error:invalid RunRayTrace arguments";
        pendingRayTrace_ = req;
        return {};
    }

    // Phase 4B item 5 "same camera asset" -- toggle between the glTF document's own first
    // Perspective camera and the live orbit camera (RayTracerApp::extractFirstCamera() result).
    if (cmd == "GetHasAssetCamera") {
        return std::string("HasAssetCamera:") + (app_ && app_->hasAssetCamera() ? "1" : "0");
    }

    if (cmd == "GetUseAssetCamera") {
        return std::string("UseAssetCamera:") + (app_ && app_->useAssetCamera() ? "1" : "0");
    }

    if (cmd.rfind("SetUseAssetCamera:", 0) == 0) {
        if (!app_) return "Error:no app";
        int v;
        if (!tryInt(cmd.substr(18), v)) return "Error:invalid int";
        // Same contract as Universe's SetUseAssetCamera: enabling with no captured asset camera
        // is a caller error, not a silent no-op; disabling is always fine.
        if (v != 0 && !app_->hasAssetCamera()) return "Error:no asset camera";
        app_->setUseAssetCamera(v != 0);
        return "OK";
    }

    // Phase 4B item 5 "report the raster/offline diff".
    if (cmd.rfind("SaveRayTraceResult:", 0) == 0) {
        if (!app_) return "Error:no app";
        const std::string path = cmd.substr(19);
        return app_->saveRayTraceResult(path) ? "OK:saved " + path : "Error:no ray trace result to save";
    }

    if (cmd.rfind("CompareImages:", 0) == 0) {
        const auto parts = splitCsv(cmd.substr(14));
        if (parts.size() != 2) return "Error:CompareImages requires pathA,pathB";
        return compareImages(parts[0], parts[1]);
    }

    return "Error:unknown command '" + cmd + "'";
}
