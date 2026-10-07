# 放物面投影による半球1回描画

## 状態

`DepthPhotonProjection::Paraboloid` を追加し、再放出点ごとの半球を1回の描画と読み戻しで取得する。5方向のヘミキューブは比較用・既定として維持する。ユーザー承認後、シェーダーコンパイルとReleaseビルド、CPU40件・GPU13件のテストが成功した。GPUスキップ0・Vulkan検証エラー0。5設定×3乱数種×3回の比較を実施した。[比較結果・画像](../../../reaserch/pbvr_photon_transport/results/2026-10-07/paraboloid_transport/README.md) を参照。同品質での高速化は未確認。

## 投影と深度

再放出面の法線を+zとする単位方向dを `uv = (d.x, d.y) / (1+d.z)` で円盤に写す。円盤内の画素中心から `d = (2u, 2v, 1-u²-v²)/(1+u²+v²)` で方向を復元する。Vulkanの画像Y向きに合わせてvの符号を変換する。

CPUで各三角形を前方半空間にクリップし、辺の中点を使って4分割を指定回数繰り返す。これによって曲線になる投影境界を直線片で近似する。元の三角形IDと法線を保存し、細分化後の三角形を1回のドローで描画する。円盤外のフラグメントは破棄する。

フラグメントでは描画中の三角形の平面から画素方向の距離を計算し、元の平面片の外側なら破棄する。頂点距離を線形補間せず、`(distance-near)/(far-near)` を `gl_FragDepth` に書く。深度テストで最前面の面を選び、この半径方向の深度から到着位置を復元する。これはラスタライズされたプリミティブ内の深度計算であり、シーン全体を探索する輸送レイやBVH照会は使わない。

細分化は投影の被覆誤差を減らす近似であり、完全な曲線ラスタライズではない。境界付近に欠落が残る可能性がある。細分化次数を増やすと1三角形当たり4^N片になり、CPU準備・アップロード・GPU描画の負荷が増える。生成100万三角形の予算を超えた場合は失敗する。

## 光束

円盤上の立体角のヤコビアンは `4/(1+u²+v²)²`。一画素の面積 `4/resolution²` を掛け、さらに `max(dot(normal,d),0)` を掛けた重みを用いる。円盤外は0。背景やクリップを含む全円盤の重みを合計1に正規化し、到着した点だけで再正規化しない。到着点保存、RGB反射、再放出点選択と確率補正はヘミキューブ方式と共通。

同じ辺長では有効画素数がヘミキューブより少なく、角度方向のサンプル配置も異なる。単純に描画回数だけを比べて、同品質の5倍高速化とは解釈しない。

## APIと実行

```cpp
DepthPhotonSettings settings;
settings.projection = DepthPhotonProjection::Paraboloid;
settings.paraboloidSubdivision = 3;
```

Vulkan SDK環境のシェルで新しいシェーダーもコンパイルする。

```powershell
cmd /c Phantom\RayTracer\PhotonSplatGpu\shaders\compile_shaders.bat
cmake --build Phantom/build/windows-release --target RayTracerTest PhotonSplatGpuTest CornellSurfaceTransport
.\Phantom\build\windows-release\RayTracer\RayTracerTest.exe
.\Phantom\build\windows-release\RayTracer\PhotonSplatGpuTest.exe
.\Phantom\build\windows-release\RayTracer\CornellSurfaceTransport.exe `
    --photon-depth scratch/paraboloid-photon/smoke 96 2 16 8 3 8 42 0.12 1 paraboloid 3
```

CLIには末尾に `[hemicube|paraboloid subdivision]` を追加した。従来の引数のみならヘミキューブ。CSVにはprojection/subdivisionを記録する。比較では同じ次数・光源位置サンプル数・再放出予算・乱数種・最終画像解像度を使用し、細分化次数と円盤の解像度も振って画像差・光束・時間を確認する。

新規GPUテストは最前面の遮蔽、距離からの位置復元、円盤外、半球境界のクリップ、後方の面、投影切替、1点1マップの統計、光束収支と設定範囲を対象にする。
