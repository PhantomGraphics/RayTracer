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

std::vector<CommandInfo> CommandDispatcher::commandCatalog() const {
    return {
        {"GetStatus", "", ""},
        {"GetMeshCount", "", ""},
        {"GetMaterialCount", "", ""},
        {"GetTextureCount", "", ""},
        {"GetCamDist", "", ""},
        {"SetCamDist", "float", "Orbit camera distance"},
        {"ResetCamera", "", ""},
        {"LoadFile", "path", "Load a glTF/GLB (response arrives when loaded)"},
        {"SaveScreenshot", "path", "Save the raster view as PNG (response arrives when written)"},
        {"RunRayTrace", "w,h,spp,depth", "Offline path trace (response arrives when finished)"},
        {"GetHasAssetCamera", "", ""},
        {"GetUseAssetCamera", "", ""},
        {"SetUseAssetCamera", "0|1", ""},
        {"SaveRayTraceResult", "path", "Save the last ray-trace result as PNG"},
        {"CompareImages", "pathA,pathB", "Mean/max difference of two PNGs"},
        {"StartPbvrPreview", "", "Start the built-in Cornell progressive preview"},
        {"RunPbvrDemo", "directory", "Save preview and 1/4/16/32-pass images plus an independent reference"},
        {"GetPbvrSamples", "", "Current refinement passes"},
        {"GetPbvrGeneration", "", "Accumulation reset generation"},
        {"SetPbvrYaw", "degrees", "Orbit the Cornell preview and reset accumulation"},
        {"WaitPbvrSamples", "count", "Deferred wait for refined passes, at most 32"},
        {"SetGltfPbvr", "0|1", "Toggle PBVR lighting in the native glTF viewport"},
        {"SetGltfPbvrGain", "float", "Indirect gain 0..2; direct lighting is unchanged"},
        {"GetGltfPbvrSamples", "", "Native glTF refinement passes"},
        {"GetGltfPbvrGeneration", "", "Native glTF accumulation generation"},
        {"WaitGltfPbvrSamples", "count", "Wait for native glTF PBVR refinement"},
    };
}

std::string CommandDispatcher::cmdCheckCommandCatalog() {
    // Probe with a junk argument against a detached app/renderer: a routed name answers
    // with its own validation error, only an unrouted one says "unknown command".
    // LoadFile/SaveScreenshot/RunRayTrace queue work that answers later; scenarios cover them.
    Phantom::Gltf::GltfSceneRenderer* const savedRenderer = renderer_;
    RayTracerApp* const savedApp = app_;
    renderer_ = nullptr;
    app_ = nullptr;
    std::string missing;
    for (const auto& c : commandCatalog()) {
        if (c.name == "LoadFile" || c.name == "SaveScreenshot" || c.name == "RunRayTrace") continue;
        const std::string probe = c.args.empty() ? c.name : c.name + ":x";
        if (route(probe).rfind("Error:unknown command", 0) == 0)
            missing += (missing.empty() ? "" : ",") + c.name;
    }
    renderer_ = savedRenderer;
    app_ = savedApp;
    return missing.empty() ? "OK" : "Error:unrouted catalog entries: " + missing;
}

void CommandDispatcher::dispatch(const std::string& command) {
    queue_.submit(command);
}

std::vector<std::string> CommandDispatcher::collectResponses() {
    return queue_.collectResponses();
}

void CommandDispatcher::processQueue() {
    std::queue<std::string> local = queue_.takeAll();

    while (!local.empty()) {
        std::string cmd = std::move(local.front());
        local.pop();
        const bool fromUi = takeUiMark(cmd);
        std::string resp = route(cmd);
        if (resp.empty() && fromUi) {   // answered later by a signal*(); drop that
            std::lock_guard<std::mutex> lock(deferredMutex_);
            ++silentDeferred_;
        }
        if (!resp.empty() && !fromUi) queue_.respond(std::move(resp));
    }
}

std::optional<std::filesystem::path> CommandDispatcher::takePendingLoad() {
    std::optional<std::filesystem::path> p;
    p.swap(pendingLoad_);
    return p;
}

void CommandDispatcher::pushDeferred(std::string resp) {
    {
        std::lock_guard<std::mutex> lock(deferredMutex_);
        if (silentDeferred_ > 0) { --silentDeferred_; return; }
    }
    queue_.respond(std::move(resp));
}

void CommandDispatcher::signalLoaded(bool ok, const std::string& msg) {
    pushDeferred(ok ? "OK:" + (msg.empty() ? std::string("loaded") : msg) : "Error:" + msg);
}

std::optional<std::filesystem::path> CommandDispatcher::takePendingScreenshot() {
    std::optional<std::filesystem::path> p;
    p.swap(pendingScreenshot_);
    return p;
}

void CommandDispatcher::signalScreenshotDone(bool ok, const std::string& path) {
    pushDeferred(ok ? "OK:saved " + path : "Error:screenshot failed");
}

std::optional<CommandDispatcher::RayTraceRequest> CommandDispatcher::takePendingRayTrace() {
    std::optional<RayTraceRequest> r;
    r.swap(pendingRayTrace_);
    return r;
}

void CommandDispatcher::signalRayTraceDone(bool ok, int width, int height) {
    pushDeferred(ok ? "OK:" + std::to_string(width) + "x" + std::to_string(height) : "Error:ray trace failed");
}

std::string CommandDispatcher::route(const std::string& cmd) {
    if (cmd == "CheckCommandCatalog") return cmdCheckCommandCatalog();
    if(cmd.rfind("SetGltfPbvrGain:",0)==0) {
        float value=0;
        if(!app_ || !tryFloat(cmd.substr(16),value) || value<0 || value>2) return "Error:expected gain 0..2";
        app_->setGltfPbvrGain(value); return "OK";
    }
    if(cmd.rfind("SetGltfPbvr:",0)==0) {
        int value=0;
        if(!app_ || !tryInt(cmd.substr(12),value) || (value!=0 && value!=1)) return "Error:expected 0 or 1";
        app_->setGltfPbvr(value!=0); return "OK";
    }
    if(cmd=="GetGltfPbvrSamples") return "Samples:"+std::to_string(app_?app_->gltfPbvrSamples():0);
    if(cmd=="GetGltfPbvrGeneration") return "Generation:"+std::to_string(app_?app_->gltfPbvrGeneration():0);
    if(cmd.rfind("WaitGltfPbvrSamples:",0)==0) {
        int count=0;
        if(!app_ || !tryInt(cmd.substr(20),count) || !app_->waitGltfPbvrSamples(count)) return "Error:glTF PBVR cannot wait";
        return {};
    }
    if(cmd=="StartPbvrPreview") {
        if(!app_) return "Error:no app";
        app_->startPbvrPreview(); return "OK";
    }
    if(cmd.rfind("RunPbvrDemo:",0)==0) {
        if(!app_) return "Error:no app";
        return app_->runPbvrDemo(cmd.substr(12))?std::string():"Error:PBVR demo cannot start";
    }
    if(cmd=="GetPbvrSamples") return "Samples:"+std::to_string(app_?app_->pbvrSamples():0);
    if(cmd=="GetPbvrGeneration") return "Generation:"+std::to_string(app_?app_->pbvrGeneration():0);
    if(cmd.rfind("SetPbvrYaw:",0)==0) {
        if(!app_) return "Error:no app";
        float degrees=0;
        return tryFloat(cmd.substr(11),degrees)&&app_->setPbvrYaw(degrees)?"OK":"Error:invalid PBVR yaw or demo active";
    }
    if(cmd.rfind("WaitPbvrSamples:",0)==0) {
        if(!app_) return "Error:no app";
        int count=0;
        if(!tryInt(cmd.substr(16),count)||!app_->waitPbvrSamples(count)) return "Error:invalid PBVR sample wait";
        return {};
    }

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
