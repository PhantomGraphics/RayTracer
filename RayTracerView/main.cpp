#include "pch.h"
#include "RayTracerApp.h"

#include <string_view>

int main(int argc, char* argv[])
{
    RayTracerApp app(1280, 720, "RayTracer View");

    std::string scenarioPath;
    bool        noExitOnComplete = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--run-scenario" && i + 1 < argc) {
            scenarioPath = argv[++i];
        } else if (a == "--no-exit-on-complete") {
            noExitOnComplete = true;
        } else if ((a == "--screenshot" || a == "--screenshot-frame") && i + 1 < argc) {
            // VkAppBase::parseScreenshotArgs() (called later, inside app.run())
            // consumes these separately -- without skipping the value here too,
            // this loop's a[0] != '-' branch below misreads the screenshot path
            // as a positional glTF model path and tries to load it as one.
            ++i;
        } else if (a.starts_with("--screenshot-frame=") || a.starts_with("--screenshot=")) {
            // combined --flag=value form: value is part of this same argv[i],
            // nothing to skip, just don't fall through to loadGltf below.
        } else if (a[0] != '-') {
            app.loadGltf(std::filesystem::path(argv[i]));
        }
    }

    if (!scenarioPath.empty()) {
        if (!app.loadScenario(scenarioPath)) {
            fprintf(stderr, "[Scenario] Failed to load: %s\n", scenarioPath.c_str());
            return 1;
        }
        app.setExitOnScenarioComplete(!noExitOnComplete);
    }

    app.run(argc, argv);
    return app.getExitCode();
}
