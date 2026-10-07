# PBVRフォトン輸送: 到着点の間引きと再放出

GPU用保存の省略と新しい抽出方式は [最適化のAPIと検証](pbvr_photon_optimization.md) を参照。

レイ追跡を使わない新しい輸送は [深度マップによるPBVR輸送](depth_photon_transport.md) を参照。従来のCPU輸送は比較用として残す。

ユーザー提案の「最初のフォトンの到着点をサンプルし、そこから間引いて再放出する処理を繰り返す」を `PhotonMapper` に実装した。ここでのPBVRは表面上のフォトン粒子集合を次数別に輸送する方式であり、既存の体積PBVRレンダラーをそのまま呼ぶ方式ではない。

## API

```cpp
using namespace Phantom::RayTracer;
PhotonMappingSettings settings;
settings.transport = PhotonTransport::Pbvr;
settings.photonCount = 50000;
settings.photonMaxDepth = 8;
settings.gatherRadius = 0.12;
settings.render.randomSeed = 42; // 光源放出と各粒子の反射方向
settings.pbvr.retention = 0.65;
settings.pbvr.maxParticles = 10000; // 各再放出の上限。0で無制限
settings.pbvr.randomSeed = 1234; // 間引き専用の乱数種

PhotonMapper mapper(settings);
if (!mapper.build(triangles)) return false;
// 通常のrenderLinear()/render()、またはPhotonSplatGpuに渡せる。
const auto& photons = mapper.getPhotonMap();
const auto& stages = mapper.getStats().bounces;
```

既定の `PhotonTransport::Path` は従来の光路単位のRussian roulette。`Pbvr` を明示すると到着点の集合を反射次数ごとに処理する。`retention` は有限な `(0,1]` の値に限る。`retention=1, maxParticles=0` は間引きなしの段階輸送で、従来の材質依存Russian rouletteも使わない。

## 一段の処理

1. 活動中の粒子をBVHで追跡し、到着点と入射パワーを求める。逸脱した光や裏面への入射は終了する。
2. 拡散面の到着点は**間引く前**にフォトンマップへ保存する。到着済みの光を減らさない。
3. RGB反射率を掛け、黒い面での吸収を適用する。
4. 生存確率 `p = retention` で到着点を独立に間引く。残った粒子のパワーを `1/p` 倍する。
5. 生存数 `M` が上限 `K` を超えたら、一様・非復元で `K` 点を抽出し、さらに `M/K` 倍する。先頭のK点を使う方式ではない。
6. 選んだ到着点から、拡散面なら余弦重み付き、完全鏡面なら鏡面方向へ再放出する。
7. 次の次数へ進む。最大次数では到着光のみ保存し、次の粒子を作らない。

補正は次の二段階となる。

```text
反射後:             W_reflected = W_in * reflectance
確率的間引きの後:   W_selected = W_reflected / p
上限抽出の後:       W_next = W_selected * M / K  (M > Kのとき)
```

第一段階で期待値を保ち、第二段階も第一段階の結果を条件として期待値を保つ。RGBを同じ確率で選択するため、色を個別に丸めない。高次反射で粒子がゼロになることはある。補正パワーが非有限になる場合はエラーを返す。

拡散到着点の分類は従来どおり直接光／間接光／caustic。鏡面の到着点は再放出候補に含むが、拡散密度推定用のマップには保存しない。カメラ描画の直接光は面光源の明示的サンプリングを使い、直接フォトンを二重計上しない。材質・光源・テクスチャ等の対応範囲は [PhotonMapper](photon_mapping.md) と共通。

各粒子は小さい独立乱数状態を持つ。同じ `render.randomSeed` なら、間引きを生き残った経路の反射方向は他の粒子の生死や並び順に依存しない。`pbvr.randomSeed` だけを変える反復では、同じ放出・光路標本に対する間引きノイズを平均する。光源放出自体のノイズも減らすには `render.randomSeed` も変える。

## 統計

`Photon::bounce` は最初の到着を0とする。`getStats().bounces[bounce]` は次を返す。

- `tracedRays`: この次数で追跡した粒子数。
- `arrivals`: 表向き面への到着数。鏡面も含む。
- `storedPhotons`, `storedFlux`: 間引き前に保存した拡散到着点の数とRGB入射光束。
- `continuationCandidates`: 材質反射後に非ゼロのパワーを持つ再放出候補数。
- `retainedParticles`, `reemittedFlux`: 間引き・上限抽出後の再放出数と補正済みRGB光束。

`tracedRays` は全次数の合計。`transportSeconds` は放出・追跡・到着点選択の時間、`mapBuildSeconds` は保存点の検証・KDTree構築時間、`buildSeconds` はシーンとBVHの準備も含む全体時間。

## 比較の実行

VS2026開発者シェルで既存のPhantom Release構成を使用する。

```powershell
cmake --build Phantom/build/windows-release --target CornellSurfaceTransport RayTracerTest PhotonSplatGpuTest
.\Phantom\build\windows-release\RayTracer\CornellSurfaceTransport.exe `
    --photon-pbvr scratch/pbvr-photon/example 96 50000 0.12 32 42 0.65 10000 32

# 通常のPowerShellからビルド・関連テスト・3乱数種の比較を一括実行
.\Phantom\RayTracer\CornellSurfaceTransport\run_pbvr_photons.ps1
```

引数: `--photon-pbvr out size photonCount radius directSamples emissionSeed retention cap ensembles`。
この比較CLIはVulkanが必要。輸送ライブラリ自体はCPUのみでも使用できる。

比較では同じ放出・方向標本の**間引きなし段階輸送**を `reference` とする。従来の `Path` は乱数列・生存規則が異なる独立した比較対象として `path` も保存する。固定の放出乱数種で、間引き乱数種だけを変えて反復平均する。集約は全方式で既存の [深度G-bufferとGPU加算スプラット](../PhotonSplatGpu/README.md) を使用し、受光点・半径・直接光の乱数をそろえる。

出力:

- `reference`, `path`, `pbvr_first`, `pbvr_mean`: 合成画像のPPMと線形HDRのPFM。
- 同名の `_indirect` 付きファイル: 間接光のみ。
- `pbvr_metrics.csv`: 全反復の累積平均、画像誤差、各段階の平均時間と総時間。
- `pbvr_orders.csv`: 方式別・反射次数別の粒子数とRGB光束。

画像誤差は線形間接光の相対L2。`indirect_mean_ratio` は間引きなしに対する間接光RGB合計の比。確率的な画像差があるため、このCLIは一致誤差の閾値で成功・失敗を判定しない。関連GoogleTestで光束保存と局所輝度の平均を検証する。

## 制約

今回の放出・交差判定・間引き・再放出はCPU単一スレッド。粒子集合を次数ごとに処理する構造を追加したが、その輸送のGPU化はまだ行っていない。最終集約は既存GPU加算方式であり、粒子間の深度競合による集約ではない。

上限は**再放出粒子数**に適用する。最初の放出数や、すでに保存した全次数の到着点には適用しない。到着点の統合・輝度重要度による抽出・空間分層抽出・適応半径は未実装。一様抽出は明るい局所経路を落とす可能性があり、強い間引きはノイズを増やす。反復平均で減らせるが、総計算時間は増える。同品質の速度改善を保証する方式ではない。

## 同品質の計測

`CornellSurfaceTransport/run_photon_quality.py` は、独立した高サンプル参照に対して従来方式とPBVR方式の放出数・反復数を調整する。こちらの `--photon-quality` モードは反復ごとに放出と間引きの両方の乱数種を変え、直接光の乱数種は共通に固定する。以前の `--photon-pbvr` の間引きだけを平均する比較とは目的が異なる。

全反復のマップ構築・GPU集約・平均化・直接光合成を含む時間を記録する。詳細・計測値・画像は [同等品質の結果](../../../reaserch/pbvr_photon_transport/results/2026-10-07/matched_quality/README.md) を参照。
