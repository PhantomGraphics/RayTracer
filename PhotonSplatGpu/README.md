# 深度G-bufferへのGPUフォトン加算集約

既存 `PhotonMapper` のフォトンを、Vulkanのラスタライズと加算ブレンドで集約する。`RayTracerPhotonGpu` はCPUの `RayTracerCore` と別のオプションライブラリで、Vulkanヘッダーとローダがある場合にビルドする。ウィンドウやGLFWは不要。

## 処理

1. シーンを深度テスト・深度書き込みありで描画する。D32深度とRGBA32Fの法線・三角形IDを保存する。
2. 各フォトンについて、実空間の収集球を保守的に囲む画面矩形を、インスタンス描画で展開する。
3. フラグメントで深度を参照し、受光位置を復元する。実空間距離、法線、入射方向、接平面からの距離を、KDTree版と同じ条件で判定する。
4. パワーに `reflectance / (pi * pi * radius^2)` を掛け、RGBA32Fの画像へONE/ONEで加算する。フォトン描画中の深度テストと深度書き込みは無効。重なるフォトンの寄与を全て加算する。
5. 同じGPU G-bufferを読み戻し、同じフォトン集合と半径を使ったCPU KDTree推定と比較する。

深度バッファは受光するシーンの最前面を決めるために使用する。フォトン同士で深度を競わせて1個だけ残す処理ではない。近接する別面の混入を抑える判定は、既存KDTree推定と同じ近似である。

## 実行

ルートから、VS2026・CMake・Vulkan SDKが利用できる環境で:

```powershell
.\Phantom\RayTracer\PhotonSplatGpu\run_photon_splat.ps1
```

96×96・256×256、乱数種42・43・44で逐次実行し、`scratch/cornell-photon-splat/summary.csv` に集約する。既定は20万放出、収集半径0.12、直接光32サンプル。ウィンドウなしで動作し、CLIはVulkan validationを有効にする。

単一条件:

```powershell
.\Phantom\build\windows-release\RayTracer\CornellSurfaceTransport.exe `
    --photon-splat scratch/cornell-photon-splat/example 256 200000 0.12 32 42
```

引数は `--photon-splat output size photonCount radius directLightSamples seed`。従来の `--photon` のspp引数とは用途が違う。今回は画素中心を1点で評価し、アンチエイリアスを使用しない。GPU・KDTreeとも同じ受光位置と同じ直接光の乱数列を使用する。

出力:

- `splat`・`kdtree`: 同一の直接光・放射に、それぞれの間接光推定を加えた画像。
- `splat_indirect`・`kdtree_indirect`: 間接光だけの画像。
- 各画像の `.pfm` は線形HDR、`.ppm` は従来実験と同じ表示変換。
- `splat_metrics.csv`: 条件、GPU名、CPU経過時間、GPU timestamp、間接光の線形相対L2と最大絶対差。

GLSLを変更した場合だけ `shaders/compile_shaders.bat` を実行する。同梱SPIR-Vを使えるため、通常のビルドにシェーダーコンパイラは不要。CMakeビルドは最新のシェーダーフォルダを実行ファイルの隣へコピーする。

## API

```cpp
#include "PhotonSplatGpu/PhotonSplatGpu.h"

// context and pool are initialized first, and outlive gpu.
Phantom::RayTracer::PhotonSplatGpu gpu;
if (!gpu.create(context, pool, shaderDirectory)) return false;
Phantom::Graphics::Imagef indirect;
Phantom::RayTracer::PhotonGBuffer receivers;
if (!gpu.render(triangles, mapper.getPhotonMap(), camera,
                width, height, radius, indirect, receivers,
                Phantom::RayTracer::PhotonContribution::Indirect,
                nearPlane, farPlane)) return false;
Phantom::Graphics::Imagef combined;
if (!mapper.shadeGBuffer(receivers, combined, &indirect)) return false;
```

`VulkanContext::initDevice(VK_NULL_HANDLE)` と `VulkanCommandPool::init(&context, VK_NULL_HANDLE)` が使用できる。初期化はsurfaceなしのグラフィックスキューを選択し、swapchain拡張を要求しない。既存のsurfaceあり初期化は維持する。

`render()` は同期実行し、G-bufferと間接光画像をCPUへ読み戻す。`shadeGBuffer()` は直接光と放射をCPUで加える。深度復元誤差による自己遮蔽を避けるため、直接光のシャドウレイは元の三角形の平面へ戻してから開始する。GPU集約はfloat、KDTreeの最終密度判定はdoubleなので、カーネル境界の一部に差が生じる。

## 計測の意味

- `gpu_wall_seconds`: 描画呼び出し全体。初回のターゲット・パイプライン準備、バッファ作成・アップロード、G-buffer生成、集約、同期、読み戻し、CPUでの画像・受光点作成を含む。Vulkan instance/device作成とシェーダー読み込みは含まない。
- `splat_gpu_ms`: GPU timestampによる集約パスの時間。G-buffer、アップロード、CPUへの読み戻しを含まない。
- `gbuffer_gpu_ms`・`copy_gpu_ms`: GPU側のG-bufferパス・転送時間。
- `kdtree_gather_seconds`: 同じ受光点でのCPU単一スレッド集約時間。共通G-bufferの生成は含まない。
- `two_composites_seconds`: 同じ直接光・放射を2枚へ加えるCPU処理。比較用に2回実行しており、1フレーム分の描画時間ではない。
- フォトンの放出・追跡・KDTree構築は共通で、`photon_build_seconds` として別に記録する。

この比較は同じカーネルでの集約処理の比較であり、フォトン追跡のGPU化やPBVRの全工程の高速化率を示すものではない。単発時間はドライバーのパイプラインキャッシュや負荷で変わる。

## 検証と制約

GPUテストでは、加算、最前面の深度、別平面・裏面の除外、画面外・near planeをまたぐ収集球、寄与分類、空マップ、リサイズ、傾いた面、入力失敗をKDTree推定と比較する。Vulkan validationのエラーも検査する。

現段階は拡散面の最終視点での集約に限定する。鏡面をカメラからたどる描画、少数プローブからの多重反射輸送、時間蓄積、MSAAは未実装。CPU `PhotonMapper` はフォトン追跡中の完全鏡面を扱えるが、GPU G-bufferのシーンはmetallic 0だけを受け付ける。

収集球がnear planeをまたぐ場合は全画面矩形へ拡張し、正確な実空間条件で除外する。正しさを保つ代わりに、大きな半径ではオーバードローが増える。深度はfloatの透視投影なので、near/farをシーンに合わせること。CLIの2単位シーンではnear 0.1、far 100を使う。固定半径のバイアス、接平面・法線フィルタの近似はCPU版と共通。

参考: [Image Space Photon Mapping](https://casual-effects.com/research/McGuire2009Photon/index.html)。実装は本リポジトリのフォトンと密度推定を使ったもので、同論文の全アルゴリズムを再現したものではない。
