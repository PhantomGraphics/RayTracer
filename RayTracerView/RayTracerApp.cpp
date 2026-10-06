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
    menuPanel_.setApp(this);

    dispatcher_.setDocument(&doc_);
    dispatcher_.setRenderer(&renderer_);
    dispatcher_.setApp(this);
    scenarioBrowser_.setHost(this);
    scenarioBrowser_.setDefaultFolder("scenarios");

    // Standard screen: render area + menu + Command + Outliner; the rest is
    // opened from the View menu / outliner.
    shell_.setDispatcher(&dispatcher_);
    shell_.registerPanel("RayTracer View", {0.70f, 0.00f, 0.30f, 0.66f});
    shell_.registerPanel("Ray Trace Result", {0.30f, 0.05f, 0.40f, 0.55f});
    shell_.registerPanel("Scenario Browser", {0.30f, 0.05f, 0.40f, 0.55f});
    shell_.setOutlinerProvider([this] {
        std::vector<ViewShell::OutlinerItem> items;
        if (!doc_.meshes.empty())
            items.push_back({1, "Scene: " + std::to_string(doc_.meshes.size()) + " mesh(es), " +
                                std::to_string(doc_.materials.size()) + " material(s)", "RayTracer View"});
        if (hasAssetCamera())
            items.push_back({2, std::string("Camera: glTF asset camera") + (useAssetCamera_ ? " (in use)" : ""), "RayTracer View"});
        if (resultPanel_.hasResult())
            items.push_back({3, "Ray trace result: " + std::to_string(resultPanel_.lastWidth()) + "x" +
                                std::to_string(resultPanel_.lastHeight()), "Ray Trace Result"});
        return items;
    });
    menuPanel_.setShell(&shell_);
    menuPanel_.setSubmit([this](const std::string& c) { dispatcher_.submitUi(c); });
    resultPanel_.setShell(&shell_);

    add(&renderer_);
    add(&menuPanel_);
    add(&resultPanel_);
    // The scenario browser draws through the shell in onImGui().
}

void RayTracerApp::onImGuiReady()
{
    // Context exists, imgui.ini is not read until the first frame.
    shell_.installSettings();
}

void RayTracerApp::onImGui()
{
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Quit")) getWindow().close();
            ImGui::EndMenu();
        }
        shell_.drawViewMenu();
        ImGui::EndMainMenuBar();
    }
    shell_.drawWindows();
    ::VKG::VkAppBase::onImGui();
    scenarioBrowser_.pumpQueue();
    if (shell_.beginPanel("Scenario Browser")) {
        scenarioBrowser_.drawEmbedded();
        shell_.endPanel();
    }
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

    // Single place that collects responses: first the ones for commands typed
    // into the Command window (scenario commands included -- they run through
    // it too), the rest go to the running scenario. Deferred answers (LoadFile,
    // SaveScreenshot, RunRayTrace) arrive in a later frame and are matched by order.
    auto responses = dispatcher_.collectResponses();
    shell_.consumeResponses(responses);
    shell_.setScenarioActive(runner_.isActive());
    menuPanel_.setLocked(runner_.isActive());

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
        if (runner_.tick(shell_.scenarioDispatcher(), responses)) {
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

    assetCamera_   = extractFirstCamera(doc_);
    useAssetCamera_ = false; // stay on the live orbit camera until the user opts in
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
    shell_.setPanelVisible("Ray Trace Result", true);   // show the result once a trace starts

    // Phase 4B item 5 "same camera asset": prefer the glTF's own camera when the user has
    // opted into it; otherwise use the live orbit camera (the default -- keeps the offline
    // render directly comparable to whatever the raster viewport is currently showing).
    Phantom::RayTracer::RtCameraSpec cam;
    if (useAssetCamera_ && assetCamera_) {
        cam = *assetCamera_;
    } else {
        const RtCameraParams cp = renderer_.getCameraParams();
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
    }

    Phantom::RayTracer::RenderSettings settings;
    settings.width           = width;
    settings.height          = height;
    settings.samplesPerPixel = spp;
    settings.maxDepth        = depth;

    if (!doc_.meshes.empty()) {
        // glTF loaded: convert meshes + textures + KHR_lights_punctual lights and path-trace
        auto built = buildFromGltf(doc_);
        resultPanel_.triggerRenderGltf(cam, settings,
                                       std::move(built.triangles),
                                       std::move(built.textures),
                                       std::move(built.lights));
    } else {
        // No glTF: fall back to Cornell Box demo
        resultPanel_.triggerRender(cam, settings);
    }
}

void RayTracerApp::setupCallbacks()
{
    auto& win = getWindow();
    // Camera input is ignored while ImGui owns the mouse and while a scenario
    // runs; a release is always forwarded so a drag can end.
    win.onMouseButton = [this](int btn, int action, int) {
        if (btn != 0) return;
        if (action == 1 && (ImGui::GetIO().WantCaptureMouse || runner_.isActive())) return;
        renderer_.handleMouseButton(action == 1);
    };
    win.onCursorPos = [this](double x, double y) {
        renderer_.handleMouseMove(x, y);
    };
    win.onScroll = [this](double, double dy) {
        if (ImGui::GetIO().WantCaptureMouse || runner_.isActive()) return;
        renderer_.handleScroll(dy);
    };
}
