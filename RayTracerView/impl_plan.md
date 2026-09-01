# RayTracerView 実装計画

## 目標

- デフォルト: glTF を Vulkan でリアルタイムレンダリング
- glTF ファイル読み込みダイアログ
- "Ray Trace" ボタンで現在カメラ情報を使い、別パネルにシーンをパストレース表示
- 参考実装: `VulkanPointCloudView`, `GltfViewer`, `RayTracer/RayTracer`

---

## 既存コード調査まとめ

| モジュール | 場所 | 役割 |
|-----------|------|------|
| `GltfRenderer` | `CGLib/GltfRenderer/` | glTF Vulkan描画ライブラリ（`GltfSceneRenderer : IVkSubRenderer`、軌道カメラ付き） |
| `GltfViewer` | `CGLib/GltfViewer/` | GltfRenderer を使ったスタンドアロンビューア |
| `VkAppBase` | `CGLib/VkAppBase/` | Vulkan/ImGui アプリ基底クラス |
| `PathTracer` | `RayTracer/RayTracer/` | CPU パストレーサー（Sphere・Quad・Box・BVH・PBR対応） |
| `VulkanPointCloudView` | `PointCloud/VulkanPointCloudView/` | アーキテクチャ参考 |

### GltfSceneRenderer のカメラ状態（現状 private）

```cpp
float     camTheta_, camPhi_, camDist_;
glm::vec3 camTarget_;
```

→ Phase 3 でカメラ情報を公開する accessor を追加する。

---

## ディレクトリ構成

```
RayTracer/RayTracerView/
├── pch.h / pch.cpp
├── main.cpp
├── RayTracerApp.h / .cpp         ← VkAppBase 派生、メインアプリ
├── RayTracerMenuPanel.h / .cpp   ← ファイル開く・Ray Trace ボタン
└── RayTraceResultPanel.h / .cpp  ← パストレース結果を ImGui::Image で表示
```

### 新規 Visual Studio プロジェクト

`RayTracer/RayTracerView/RayTracerView.vcxproj`

- ToolsVersion / PlatformToolset: v143（RayTracer.vcxproj に合わせる）
- C++ 標準: C++20
- 依存プロジェクト参照: `VkAppBase`, `GltfRenderer`, `RayTracer`（静的ライブラリ化が必要なら分離）
- インクルードパス: `$(SolutionDir)` + Vulkan SDK, GLFW, GLM, Dear ImGui

---

## 実装フェーズ

---

### Phase 1 — プロジェクト scaffold

**目標:** ビルドが通るスケルトン

1. `RayTracerView.vcxproj` 作成（RayTracer.vcxproj をコピーして改変）
2. `pch.h` / `pch.cpp` 作成
3. `main.cpp` 作成 — GltfViewerApp の main.cpp に相当

```cpp
// main.cpp スケッチ
int main(int argc, char* argv[]) {
    try {
        RayTracerApp app(1280, 720, "RayTracer View");
        if (argc > 1) app.loadGltf(argv[1]);
        app.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
```

4. `RayTracerApp` 骨格作成（`onInit`/`onCleanup` のみ実装）

**完了条件:** リンクエラーなし、ウィンドウが開く

---

### Phase 2 — glTF リアルタイムビュー

**目標:** glTF ファイルを Vulkan で表示する

1. `RayTracerApp` に `GltfDocument doc_` と `GltfSceneRenderer renderer_` を追加
2. `loadGltf(path)` 実装 — `GltfReader::load()` → `renderer_.setDocument(doc_)`
3. `onInit` で `add(&renderer_)` 登録
4. `onSwapChainCreated` で `renderer_.setExtent(getExtent())` 呼び出し
5. マウス/スクロールコールバックを `renderer_` に転送（GltfViewer の `setupCallbacks()` を参考）

**完了条件:** glTF ファイルを引数に渡すと Vulkan で表示される

---

### Phase 3 — ファイル開くダイアログ + メニューパネル

**目標:** UI からファイル読み込みができる

1. `RayTracerMenuPanel` クラス実装 — `IVkUIPanel` 派生
   - `ImGui::BeginMainMenuBar()` → "File" → "Open glTF..." メニュー項目
   - ファイルパス入力 + "Open" ボタン（`FileOpenView` を流用可能）
   - "Ray Trace" ボタン（Phase 4 で実装、ここではグレーアウト）
2. `RayTracerApp::onImGui` でパネルを描画
3. ファイル開くコールバックで `loadGltf()` を呼び出す

**完了条件:** UI からダイアログを開いて glTF ファイルを読み込める

---

### Phase 4 — カメラ情報抽出

**目標:** Vulkan レンダラーのカメラ状態を PathTracer に渡せるようにする

1. `CGLib/GltfRenderer/Renderer/GltfSceneRenderer.h` に公開メソッド追加

```cpp
struct RtCameraParams {
    glm::vec3 eye;
    glm::vec3 target;
    glm::vec3 up;
    float     fovDegVertical;
};
RtCameraParams getCameraParams() const;
```

2. `GltfSceneRenderer.cpp` に実装（既存の `cameraPosition()` 相当処理を使用）

```cpp
RtCameraParams GltfSceneRenderer::getCameraParams() const {
    const float sinPhi = std::sin(camPhi_);
    const float cosPhi = std::cos(camPhi_);
    const glm::vec3 eye = camTarget_ + camDist_ * glm::vec3(
        sinPhi * std::cos(camTheta_),
        std::cos(camPhi_),   // or cosPhi
        sinPhi * std::sin(camTheta_));
    return { eye, camTarget_, {0.f, 1.f, 0.f}, 60.f };
}
```

（正確な式は既存の `computeMVP()` / `cameraPosition()` を確認して合わせる）

**完了条件:** `renderer_.getCameraParams()` で eye/target/up を取得できる

---

### Phase 5 — PathTracer バックグラウンド実行 + 進捗表示

**目標:** "Ray Trace" ボタン押下でバックグラウンドでパストレースを実行

1. `RayTraceResultPanel` クラス作成
   - `triggerRender(const RtCameraParams&, const RenderSettings&)` — `std::async` でパストレース起動
   - `onImGui()` — 進捗バー + 完了後に画像表示
   - 内部: `std::future<Graphics::Imageuc>` を保持

2. `RayTracerApp::onRayTrace()` コールバック実装
   ```cpp
   void RayTracerApp::onRayTrace() {
       auto cam  = renderer_.getCameraParams();
       RenderSettings rs; // デフォルト設定 or UI から取得
       resultPanel_.triggerRender(cam, rs);
   }
   ```

3. 初期段階では `renderCornellBox()` を呼び出し、カメラは無視してよい（動作確認優先）

4. メニューパネルの "Ray Trace" ボタンからコールバックを呼ぶ

**完了条件:** Ray Trace ボタンでコーネルボックスがレンダリングされる（glTF とは無関係でよい）

---

### Phase 6 — 結果画像の ImGui 表示

**目標:** レンダリング結果を ImGui パネルにテクスチャとして表示

1. `Graphics::Imageuc` → `VkImage` へアップロードするユーティリティ関数作成

```cpp
// レンダリング結果を VkImage にアップロード
VkDescriptorSet uploadImageToImGui(const Graphics::Imageuc& img, ...);
```

2. `ImGui_ImplVulkan_AddTexture()` を使い `ImTextureID` を取得
3. `RayTraceResultPanel::onImGui()` 内で `ImGui::Image(texId, size)` で表示
4. 再レンダリング時は古いテクスチャを破棄し新しいものに差し替え

**完了条件:** Ray Trace 後に別パネルに画像が表示される

---

### Phase 7 — glTF メッシュ → PathTracer シーン変換

**目標:** glTF モデルを PathTracer で正確にレイトレーシングする

1. `PathTracer.h/cpp` に `Triangle` Hittable を追加

```cpp
class Triangle : public Hittable {
public:
    Triangle(const Vec3& v0, const Vec3& v1, const Vec3& v2,
             std::shared_ptr<Material> mat);
    bool hit(const Ray&, double tMin, double tMax, HitRecord&) const override;
    Math::Box3df getAABB() const override;
private:
    Vec3 v0_, v1_, v2_, normal_;
    std::shared_ptr<Material> mat_;
};
```

（Möller–Trumbore 交差アルゴリズム）

2. `GltfToHittableList` 変換関数を `RayTracerView` 内に実装

```cpp
// GltfDocument → HittableList (Triangle の集合)
HittableList buildSceneFromGltf(const GltfDocument& doc);
```

- `GltfAccessorView` で POSITION / NORMAL / INDICES を取得
- glTF の metallic-roughness マテリアルを `PbrMaterial` にマッピング
- ノードのワールド変換行列を適用

3. `onRayTrace()` で `buildSceneFromGltf(doc_)` を呼び出し、BVH構築 → レンダリング

4. `PathTracer.h` に新しいエントリポイントを追加

```cpp
struct RtCameraSpec {
    Vector3dd lookFrom, lookAt, up;
    double fovDeg;
};
bool renderHittableList(const HittableList& scene, const RtCameraSpec& cam,
                        const RenderSettings& settings, Graphics::Imageuc& output);
```

**完了条件:** glTF モデルが PathTracer でレイトレースされる

---

### Phase 8 — レンダリング設定 UI

**目標:** 解像度・SPP・最大深度を UI で設定できる

1. `RayTracerMenuPanel` / `RayTraceResultPanel` に設定スライダーを追加
   - Width / Height
   - Samples Per Pixel
   - Max Depth
   - Seed

---

## ファイル依存関係図

```
RayTracerView
├── VkAppBase           (lib)
├── GltfRenderer        (lib) ← GltfSceneRenderer, GltfDocument, GltfReader
└── RayTracer           (lib) ← PathTracer, renderCornellBox, renderHittableList
    └── CGLib           (lib) ← Image, BVH, Math
```

---

## 実装状況

| Phase | 状態 | 完了日 | 備考 |
|-------|------|--------|------|
| 1 プロジェクト scaffold | ✅ 完了 | 2026-05-11 | vcxproj, pch, main.cpp, sln登録 |
| 2 glTF Vulkan表示 | ✅ 完了 | 2026-05-11 | GltfSceneRenderer流用、マウス/スクロール対応 |
| 3 ファイル開くダイアログ | ✅ 完了 | 2026-05-11 | RayTracerMenuPanel実装 |
| 4 カメラ抽出 | ✅ 完了 | 2026-05-11 | GltfSceneRenderer::getCameraParams()追加 |
| 5 PathTracer バックグラウンド実行 | ✅ 完了 | 2026-05-11 | std::async + renderCornellBoxWithCamera() |
| 6 結果画像表示 | ✅ 完了 | 2026-05-11 | RayTraceResultPanel + ImGui_ImplVulkan_AddTexture |
| 7 glTF→シーン変換 | ✅ 完了 | 2026-05-12 | Triangle Hittable + GltfSceneBuilder + renderTriangleScene() |
| 8 設定UI | ✅ 完了 | 2026-05-11 | Width/Height/SPP/Depth スライダー |

### 作成ファイル (Phase 1-6)

**新規ファイル:**
- `RayTracer/RayTracerView/pch.h`, `pch.cpp`
- `RayTracer/RayTracerView/main.cpp`
- `RayTracer/RayTracerView/RayTracerApp.h`, `.cpp`
- `RayTracer/RayTracerView/RayTracerMenuPanel.h`, `.cpp`
- `RayTracer/RayTracerView/RayTraceResultPanel.h`, `.cpp`
- `RayTracer/RayTracerView/RayTracerView.vcxproj`

**変更ファイル:**
- `CGLib/GltfRenderer/Renderer/GltfSceneRenderer.h` — `RtCameraParams`構造体, `getCameraParams()`追加
- `CGLib/GltfRenderer/Renderer/GltfSceneRenderer.cpp` — `getCameraParams()`実装
- `RayTracer/RayTracer/PathTracer.h` — `RtCameraSpec`構造体, `renderCornellBoxWithCamera()`追加
- `RayTracer/RayTracer/PathTracer.cpp` — `renderCornellBoxWithCamera()`実装
- `Phantom2026.sln` — RayTracerViewプロジェクト登録

### Phase 7完了後のレイトレース動作
- glTFファイルが読み込まれている場合: GltfSceneBuilder でメッシュをトライアングルリストに変換し、BVH + パストレースで描画（空は sky gradient 背景）
- glTFが未読み込みの場合: コーネルボックスをフォールバック表示

**追加ファイル:**
- `RayTracer/RayTracerView/GltfSceneBuilder.h`, `GltfSceneBuilder.cpp` — glTF → RtTriangle 変換
- `PathTracer.h`: `RtTriangle` 構造体, `renderTriangleScene()` 追加
- `PathTracer.cpp`: `Triangle` Hittable (Möller–Trumbore), `gradientSky()`, `traceGltf()`, `renderTriangleScene()` 追加

---

## 実装優先順位 (MVP)

| Phase | 優先度 | 見積もり工数 |
|-------|-------|------------|
| 1 プロジェクト scaffold | 必須 | 0.5日 |
| 2 glTF Vulkan表示 | 必須 | 0.5日 |
| 3 ファイル開くダイアログ | 必須 | 0.5日 |
| 4 カメラ抽出 | 必須 | 0.5日 |
| 5 PathTracer バックグラウンド実行 | 必須 | 1日 |
| 6 結果画像表示 | 必須 | 1日 |
| 7 glTF→シーン変換 | 重要 | 2日 |
| 8 設定UI | 任意 | 0.5日 |

**合計目安: 6〜7日**

---

## 注意事項・既知の課題

1. **RayTracer プロジェクトの静的ライブラリ化:** 現在 `RayTracer.vcxproj` は Console Application。`PathTracer.h/cpp` を使うには、StaticLibrary として分離するか、RayTracerView プロジェクトに直接ソースを追加する必要がある。

2. **GltfSceneRenderer の `cameraPosition()` が private:** Phase 4 で `getCameraParams()` を public 追加する。既存の GltfViewer への影響なし（追加のみ）。

3. **ImGui テクスチャのライフタイム管理:** `ImGui_ImplVulkan_AddTexture` で作成した `VkDescriptorSet` はアプリ終了時に明示的に解放が必要。

4. **スレッドセーフ:** PathTracer は `std::async` で実行、完了後 main スレッドでテクスチャをアップロードする。ImGui 描画中にフューチャーをポーリングする。

5. **glTF シーンスケール:** PathTracer のコーネルボックスは単位が 0〜555 。glTF はメートル単位が多い。変換時にスケーリングが必要な場合がある。
