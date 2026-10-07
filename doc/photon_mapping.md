# PhotonMapper: CPUフォトンマッピング

`RayTracerCore` に `RayTracer/PhotonMapper.h`・`PhotonMapper.cpp` を追加した。実験の表面粒子キャッシュとは別に、光源から光路を追跡してフォトンを蓄積する二段階のフォトンマッピングである。

## API

```cpp
#include "RayTracer/PhotonMapper.h"

using namespace Phantom::RayTracer;
PhotonMappingSettings settings;
settings.photonCount = 200000;
settings.photonMaxDepth = 8;
settings.gatherRadius = 0.12; // Scene world units.
settings.render.width = 256;
settings.render.height = 256;
settings.render.samplesPerPixel = 16;

PhotonMapper mapper(settings);
// triangles and camera use the existing RtTriangle / RtCameraSpec types.
if (!mapper.build(triangles)) {
    // Inspect mapper.getLastError().
    return false;
}
Phantom::Graphics::Imagef linear;
if (!mapper.renderLinear(camera, linear)) return false;
const auto& stats = mapper.getStats();
```

`build()` はシーン・フォトンマップを作り直す。失敗時は既存マップを無効化する。`renderLinear()` は線形HDR RGBAを返し、`render()` は既存PathTracerと同様に0〜1へのクランプと平方根のガンマ補正をした8bit RGBAを返す。失敗した描画は出力画像を変更しない。カメラ変更だけならマップを再利用できる。形状・材質・光源変更後は再構築する。

`getPhotonMap()` から保存フォトンを参照できる。`PhotonMap` は単独でも使用でき、外部で生成したフォトンを `build()` で設定し、`estimateRadiance()` で固定半径の推定を行える。`Photon::power` は放出数で正規化済みのRGBフラックス、`direction` は入射光の伝播方向、`normal` は衝突面の単位法線。

## 対応シーン

- `RtTriangle` の位置・RGB反射率・RGB放射輝度を使用する。
- `metallic == 0`: Lambert拡散反射。`roughness` はこの拡散モデルには使わない。
- `metallic == 1 && roughness == 0`: RGB反射率を持つ完全鏡面。
- 放射は三角形の頂点順から決まる法線側のみ。面の裏側は遮蔽物として扱い、反射・放射しない。
- 反射率は各成分0〜1。放射輝度は有限かつ非負。ゼロ面積の三角形は受け付けない。
- 発光三角形が最低1枚必要。`RtLight` の点光源・方向光源はこのAPIにはまだ対応していない。
- テクスチャ、粗い金属、中間のmetallic、屈折、透過、参加媒質は未対応。対応外の入力は黙って近似せず、`build()` で失敗を返す。

完全鏡面を経由した光源→鏡面→拡散面の経路はcausticとして分類する。ただし、ガラスの屈折によるコースティクスは未対応。

## アルゴリズム

1. 三角形の面積と放射輝度の輝度値で光源選択確率 `p_light` を求める。
2. 光源の表面を面積一様、放射方向を余弦重み付きでサンプルする。初期パワーは `pi * area * emission / (emittedCount * p_light)`。
3. BVH候補検索と三角形交差判定で光路を追跡する。
4. 拡散面の衝突点に、反射前の入射パワーを保存する。初回直接光、間接光、鏡面後の初回拡散衝突を区別する。
5. 最大RGB反射率を生存確率とするロシアンルーレットを行い、生存時は `reflectance / survivalProbability` でパワーを補正する。
6. 保存位置を既存 `Space::KDTree` で索引化する。
7. 描画では、直接光を面光源の明示的サンプリングで計算し、間接光・causticだけをフォトンマップから取得する。直接フォトンは二重計上しない。カメラから見える鏡面はレイを反射して評価する。

固定半径・一様カーネルの拡散出射輝度は次で推定する。

```text
L_out = reflectance / pi * sum(incidentPhotonPower) / (pi * radius^2)
```

フォトンは入射フラックスを持つため、ここで余弦をもう一度掛けない。`PhotonContribution::Indirect` による検索はcausticも含む。`getStats().indirectPhotons` はcausticを除いた個数であり、`causticPhotons` と別に報告する。

## コーネルボックスの実行例

VS2026開発者シェルで:

```powershell
cmake --build Phantom/build/windows-release --target RayTracerTest CornellSurfaceTransport
.\Phantom\build\windows-release\RayTracer\RayTracerTest.exe
.\Phantom\build\windows-release\RayTracer\CornellSurfaceTransport.exe `
    --photon scratch/cornell-photon 96 200000 0.12 32 42
```

PhantomのRelease構成が未設定なら先に `Phantom` ディレクトリで `cmake --preset windows-release` を実行する。コマンドの引数は `--photon output size photonCount gatherRadius spp seed`。

出力は `photon.pfm`（線形HDR）、`photon.ppm`（既存実験と共通の表示変換）、`photon_metrics.csv`。実験の2単位コーネルボックスを三角形化して、ライブラリAPIを呼んでいる。画像の左右方向は従来の実験出力にそろえるためCLI側で反転する。ライブラリ自体は通常のカメラ基底を使用する。

## 検証と制約

GoogleTestでフラックスの正規化、余弦の二重適用防止、面・寄与のフィルタ、複数光源の確率補正、ロシアンルーレットのRGB補正、直接光の二重計上防止、鏡面経路、乱数の再現性、入力・再構築失敗を検証する。

推定は固定半径なのでバイアスがある。半径を小さくするとノイズが増え、大きくすると細部がぼける。法線の一致と接平面からの距離で近隣面の混入を抑えるが、輪郭でのカーネル切断や、近接する同一平面の別領域の光漏れは完全には解決していない。

KDTreeとBVHはfloat座標、交差判定と保存パワーはdoubleである。非常に大きい座標で微小な形状を扱う場合は、シーンを適切な尺度へ変換する必要がある。輸送はCPU単一スレッド、推定は固定半径版。progressive photon mapping、final gathering、適応半径、フォトン輸送のGPU化は未実装。

初回の96×96、20万放出、32 sppの合成コーネルボックスで描画を確認した。保存数は359,490（直接161,653、間接197,837、caustic 0）。1本の光路が複数の拡散衝突を保存するため、保存数は放出数を超え得る。速度・誤差対品質の本格比較は今後の評価とする。

理論の参考: [Jensenほか: A Practical Guide to Global Illumination using Photon Mapping](https://graphics.stanford.edu/courses/cs348b-01/course8.pdf)。

## GPUでの深度G-buffer集約

[PhotonSplatGpu](../PhotonSplatGpu/README.md) は、同じフォトンを深度G-bufferへ加算スプラットし、同じ受光点でKDTree推定と比較するオプションのVulkanライブラリ。フォトン追跡は引き続きCPUで行う。

## 到着点を間引くPBVRフォトン輸送

`settings.transport = PhotonTransport::Pbvr` で、反射次数ごとの到着点保存・間引き・パワー補正・再放出を選択できる。詳細と比較CLIは [PBVRフォトン輸送](pbvr_photon_transport.md) を参照。
