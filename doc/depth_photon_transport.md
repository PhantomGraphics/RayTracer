# 深度マップによるPBVRフォトン輸送

## 状態

CPUでレイを追跡する従来PBVR輸送に代わる、Vulkanラスタライズによる輸送を追加した。実行入口は `--photon-depth`、ライブラリAPIは `DepthPhotonTransport`。従来の `--photon-pbvr` と `PhotonTransport::Pbvr` は比較用のCPU輸送として維持する。

ユーザー承認後にReleaseビルド、CPU40件・GPU11件のテスト、小規模レンダリングを実施した。全テスト成功、GPUスキップ0・Vulkan検証エラー0。画像確認で新しいCLIの面の向き補正漏れを修正し、再ビルド・再描画した。[小規模検証結果](../../../reaserch/pbvr_photon_transport/results/2026-10-07/depth_transport/README.md) を参照。同品質で従来フォトンマッピングを上回るかどうかは未検証。

## 輸送

1. 発光三角形を面積×輝度に応じて選び、三角形内を一様にサンプルして発光点を作る。
2. 各点の法線方向の半球を、90度の上面と4側面の深度・面ID・法線マップで覆う。
3. 深度テストで最前面になった画素を到着点として復元する。フォトン輸送ではBVH交差判定を呼ばない。
4. 到着光束をフォトンマップへ保存してから、到着面のRGB反射率を掛ける。
5. 到着点から再放出点を選択し、選択確率で補正した光束を持たせる。
6. 次の次数では解像度を下げて繰り返す。最終的には既存GPUスプラットでカメラのG-bufferへ集約する。

三角形光源の各サンプルの光束は `pi * area * emission / (lightSamples * lightProbability)`。画素の方向をd、再放出法線をnとすると、光束の重みは `max(dot(n,d),0) * deltaSolidAngle`。90度面上の画素中心(u,v)では `deltaSolidAngle ≈ 4 / (resolution² * (1+u²+v²)^(3/2))` とする。

離散化した半球の全画素の重みを合計1に正規化する。背景・裏面・near/farクリップで到着しない方向も、この正規化の分母に含める。到着した画素だけで正規化して光を増やすことはしない。各次数の `launchedFlux = arrivedFlux + escapedFlux` を記録する。escapedには実際の背景だけでなく、クリップや裏面による未到着も含む。

位置や異なる面の光束を平均化しない。パワー・空間パワー抽出は既存 `resamplePbvr` を使い、反射後光束の期待値を補正する。この深度方式の `Uniform` は固定予算の層化一様抽出であり、従来CPU方式のBernoulli間引きとは異なる。重複して選んだ同一点は同じ深度マップになるため、この方式では複製による独立方向サンプルの利点はない。

## API

`RayTracerPhotonGpu` にリンクし、作成済みの `PhotonSplatGpu` を渡す。

```cpp
DepthPhotonSettings settings;
settings.lightSamples = 8;
settings.maxDepth = 4;
settings.firstResolution = 32;
settings.minResolution = 8;
settings.resolutionDropEvery = 1; // 32 -> 16 -> 8 -> 8
settings.selection.maxParticles = 64;
settings.selection.selection = PbvrSelection::Power;

DepthPhotonTransport transport;
PhotonMap photons;
if (!transport.build(gpu, triangles, settings, photons)) {
    // transport.getLastError()
}
```

`resolutionDropEvery=0` は全次数同解像度。SpatialPowerには正の `spatialCellSize` を明示する。既定ではGPU集約向けに索引と直接光フォトンの保存を省略する。保存・候補数の予算を超えたら失敗し、光を黙って切り捨てない。失敗時はマップと統計をクリアする。

`PhotonMapper::buildWithPhotons` は輸送済みの点を取り込んで最終シェーディングを準備し、フォトンの放出や追跡を行わない。コーネル比較CLIではこれに空のマップを渡し、GPU間接光画像を `shadeGBuffer` で合成する。最終画像の直接照明は比較条件を維持するため従来のCPUシャドウレイを使用する。これはフォトン輸送とは別であり、画像生成全体からレイ処理を除去したという意味ではない。

## 検証の実行手順

VS2026開発者シェル、既存のPhantom Release構成でビルドし、CPUとGPUの関連テストを実行する。最初の小規模画像は以下で生成できる。

```powershell
cmake --build Phantom/build/windows-release --target RayTracerTest PhotonSplatGpuTest CornellSurfaceTransport
.\Phantom\build\windows-release\RayTracer\RayTracerTest.exe --gtest_filter=PhotonMapperTest.*:PhotonMapTest.*
.\Phantom\build\windows-release\RayTracer\PhotonSplatGpuTest.exe
.\Phantom\build\windows-release\RayTracer\CornellSurfaceTransport.exe `
    --photon-depth scratch/depth-photon/smoke 96 2 16 8 3 8 42 0.12 1
```

引数は `out imageSize lightSamples firstResolution minResolution maxDepth cap seed radius dropEvery`。出力は `depth` と `depth_indirect` のPPM/PFM、`depth_orders.csv`、`depth_metrics.csv`。全体時間には輸送・最終GPU集約・直接光合成を含むが、Vulkan初期化と画像保存は含まない。初回パイプライン作成は含むため、過去のウォームアップ後の時間と直接比較しない。

新規テストには最前面の遮蔽、深度専用から通常集約への切替、光束の収支、解像度低下、再放出上限、再現性、予算超過、未対応入力、外部フォトン取り込み時の輸送省略を含む。これらを通してから、同解像度版と低解像度版を同品質で比較する。

## 現段階の限界

拡散・不透明・テクスチャなしの面が対象。鏡面、透過、コースティクスは対応していない。光源面の位置サンプル数、角度方向の低解像度、near/farと再放出位置のオフセットは近似誤差や光漏れに影響する。

深度テストはGPUで行うが、結果を各描画後にCPUへ読み戻し、CPUで光束と再放出点を選択する。5方向の描画と同期を再放出点ごとに繰り返すため、高速化は未確認。バッファは再利用するが、三角形の再アップロードや同期は残る。多視点の配列描画・GPU内選択は今後の改善候補。
