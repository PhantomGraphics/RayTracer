#include "pch.h"
#include "RayTracerApp.h"
#include "GltfSceneBuilder.h"

#include "../../CGLib/GltfRenderer/Gltf/GltfReader.h"
#include "../../CGLib/VulkanGraphics/VulkanSPVResolver.h"

using namespace Phantom::Gltf;

RayTracerApp::RayTracerApp(int width, int height, const std::string& title)
    : ::VKG::VkAppBase(width, height, title)
{
    menuPanel_.init(
        &renderer_,
        [this](const std::filesystem::path& p) { pendingPath_ = p; },
        [this](int w, int h, int spp, int d)   { onRayTrace(w, h, spp, d); });

    dispatcher_.setDocument(&doc_);
    dispatcher_.setRenderer(&renderer_);
    scenarioBrowser_.setHost(this);
    scenarioBrowser_.setDefaultFolder("scenarios");

    add(&renderer_);
    add(&menuPanel_);
    add(&resultPanel_);
    add(&scenarioBrowser_);
}

void RayTracerApp::onInit()
{
    applyShaders();
    ::VKG::VkAppBase::onInit();
    renderer_.setExtent(getExtent());
    resultPanel_.init(&getContext(), &getCommandPool());
    setupCallbacks();
}

void RayTracerApp::applyShaders()
{
    GltfSceneRenderer::Shaders s;
    s.vertSpv       = ::VKG::loadSPVRepo("shaders/gltf.vert.spv");
    s.fragSpv       = ::VKG::loadSPVRepo("shaders/gltf.frag.spv");
    s.skyboxVertSpv = ::VKG::loadSPVRepo("shaders/skybox.vert.spv");
    s.skyboxFragSpv = ::VKG::loadSPVRepo("shaders/skybox.frag.spv");
    renderer_.setShaders(std::move(s));
}

void RayTracerApp::onUpdate(uint32_t frameIndex)
{
    if (pendingPath_) {
        reloadFile(*pendingPath_);
        pendingPath_.reset();
    }
    // Sync rendering state to menu panel (for button disable)
    menuPanel_.setRendering(resultPanel_.isRendering());

    dispatcher_.processQueue();

    if (auto p = dispatcher_.takePendingLoad()) {
        const bool ok = std::filesystem::exists(*p);
        if (ok) reloadFile(*p);
        dispatcher_.signalLoaded(ok, ok ? "" : "file not found: " + p->string());
    }

    if (screenshotPending_ && isScreenshotDone()) {
        dispatcher_.signalScreenshotDone(true, screenshotPendingPath_);
        screenshotPending_ = false;
    }
    if (auto p = dispatcher_.takePendingScreenshot()) {
        std::filesystem::create_directories(p->parent_path());
        screenshotPendingPath_ = p->string();
        screenshotPending_     = true;
        requestScreenshot(screenshotPendingPath_);
    }

    if (rayTracePending_ && !resultPanel_.isRendering()) {
        dispatcher_.signalRayTraceDone(true, resultPanel_.lastWidth(), resultPanel_.lastHeight());
        rayTracePending_ = false;
    }
    if (auto req = dispatcher_.takePendingRayTrace()) {
        onRayTrace(req->width, req->height, req->spp, req->depth);
        rayTracePending_ = true;
    }

    if (runner_.isActive()) {
        auto responses = dispatcher_.collectResponses();
        if (runner_.tick(dispatcher_, responses)) {
            if (runner_.hasFailed()) {
                fprintf(stderr, "[Scenario] FAILED: %s\n", runner_.failMessage().c_str());
                exitCode_ = 1;
            } else {
                fprintf(stdout, "[Scenario] PASSED (%zu steps)\n", runner_.stepCount());
                exitCode_ = 0;
            }
            if (exitOnComplete_) getWindow().close();
        }
    } else {
    }

    ::VKG::VkAppBase::onUpdate(frameIndex);
}

void RayTracerApp::onSwapChainCreated()
{
    renderer_.setExtent(getExtent());
}

void RayTracerApp::onCleanup()
{
    resultPanel_.cleanup(getDevice());
    ::VKG::VkAppBase::onCleanup();
}

void RayTracerApp::loadGltf(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) {
        fprintf(stderr, "RayTracerView: file not found: '%s'\n",
                path.string().c_str());
        return;
    }
    auto doc = GltfReader::load(path);
    if (!doc) {
        fprintf(stderr, "RayTracerView: failed to load glTF: '%s'\n", path.string().c_str());
        return;
    }
    doc_ = std::move(*doc);
    renderer_.setDocument(doc_);
    menuPanel_.setFilePath(path);
}

void RayTracerApp::reloadFile(const std::filesystem::path& path)
{
    vkDeviceWaitIdle(getDevice());
    renderer_.onCleanup(getDevice());
    loadGltf(path);
    applyShaders();
    renderer_.onInit(getContext(), getCommandPool(), getRenderPass(), MAX_FRAMES_IN_FLIGHT);
}

void RayTracerApp::onRayTrace(int width, int height, int spp, int depth)
{
    const RtCameraParams cp = renderer_.getCameraParams();

    Phantom::RayTracer::RtCameraSpec cam;
    cam.lookFrom[0] = cp.eye.x;
    cam.lookFrom[1] = cp.eye.y;
    cam.lookFrom[2] = cp.eye.z;
    cam.lookAt[0]   = cp.target.x;
    cam.lookAt[1]   = cp.target.y;
    cam.lookAt[2]   = cp.target.z;
    cam.up[0]       = cp.up.x;
    cam.up[1]       = cp.up.y;
    cam.up[2]       = cp.up.z;
    cam.fovDeg      = static_cast<double>(cp.fovDeg);

    Phantom::RayTracer::RenderSettings settings;
    settings.width           = width;
    settings.height          = height;
    settings.samplesPerPixel = spp;
    settings.maxDepth        = depth;

    if (!doc_.meshes.empty()) {
        // glTF loaded: convert meshes to triangles + textures and path-trace
        auto built = buildFromGltf(doc_);
        resultPanel_.triggerRenderGltf(cam, settings,
                                       std::move(built.triangles),
                                       std::move(built.textures));
    } else {
        // No glTF: fall back to Cornell Box demo
        resultPanel_.triggerRender(cam, settings);
    }
}

void RayTracerApp::setupCallbacks()
{
    auto& win = getWindow();
    win.onMouseButton = [this](int btn, int action, int) {
        if (btn == 0) renderer_.handleMouseButton(action == 1);
    };
    win.onCursorPos = [this](double x, double y) {
        renderer_.handleMouseMove(x, y);
    };
    win.onScroll = [this](double, double dy) {
        renderer_.handleScroll(dy);
    };
}
