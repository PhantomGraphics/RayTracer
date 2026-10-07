# 表面粒子によるコーネルボックスの間接照明実験

PBVRの粒子プローブ散乱から着想したCPU実験。表面粒子に出射輝度を保持し、反射次数ごとに収集・補間・累積する。体積用の `ParticleProbeScattering` を直接呼ぶものではなく、表面のLambert反射に置き換えた最初の検証実装である。RayTracerのCMakeに `CornellSurfaceTransport` 実行ターゲットを登録している。

## 実行

WindowsではVS2026のC++ツール、CMake、同梱Ninjaを使用する。ルートから:

```powershell
.\Phantom\RayTracer\CornellSurfaceTransport\run.ps1
```

既定では乱数種42・43・44について、高次反射のサンプル削減あり／なしの計6回を実行する。出力はルートの `scratch/cornell-results/`。PhantomのRelease構成を設定し、このターゲットと必要な依存だけをビルドする。実行ファイルは `Phantom/build/windows-release/RayTracer/CornellSurfaceTransport.exe`。

Linuxや開発者シェルからは通常のCMakeを使用できる。

```text
cmake -S Phantom/RayTracer -B scratch/raytracer_surface_linux -DCMAKE_BUILD_TYPE=Release
cmake --build scratch/raytracer_surface_linux --target CornellSurfaceTransport
ctest --test-dir scratch/raytracer_surface_linux -R '^CornellSurfaceTransport\.' --output-on-failure
```

実行ファイルの位置引数:

```text
CornellSurfaceTransport [out size grid samples orders referenceSpp seed reduce displaySpp]
```

`grid` は各面の一辺あたりの粒子数。`orders` は直接照明の後に追加する間接反射次数。`reduce=1` は反射次数ごとの予算を `samples, samples/2, samples/4, ...`（最小4）にする。`reduce=0` は全次数に同じ予算を使用する。

## アルゴリズム

- 単位のそろった合成シーン: 一辺2の箱、赤・緑の壁、白い床・天井・背面、軸に平行な白い箱2個、面積0.36・放射輝度12の天井光源。公式Cornell測定データの再現ではない。
- 各面の格子セル中心に表面粒子を置き、面光源の直接照明を計算する。
- 各粒子から余弦重み付き半球方向をサンプルし、最初に当たる面の前次数の出射輝度を双線形補間する。BRDFと余弦をPDFで割った結果はRGB反射率となる。
- 前次数だけを伝播させ、次数別の寄与を累積する。毎次数の乱数を変える。
- 遮蔽は有限四角形への交差判定で計算する。面の裏側からは寄与を受けない。
- 補間は同一面内に限定する。描画時の直接照明は画素ごとに計算し、間接光だけをキャッシュから得る。
- 比較用パストレーサーは同じ形状・BRDF・光源を使うが、粒子キャッシュを使用せず、各画素から反射経路をサンプルする。両方式の反射次数をそろえる。直接光を明示的にサンプルするため、反射後の光源ヒットは加算せず二重計上を避ける。

## 出力と評価

`direct`（直接照明のみ）、`surface`（表面粒子方式）、`reference`（パストレーシング）の各画像を保存する。

- `.pfm`: 線形RGB浮動小数値。誤差評価用。下の行から保存する標準PFM形式。
- `.ppm`: RGB8。共通の `L/(1+L)` とガンマ2.2で表示用に変換。
- `metrics.csv`: 計算条件、照明計算時間、描画時間、参照計算時間、線形RMSE、相対L2、平均RGB、間接光の平均RGB。
- `diffuse_relative_l2`: 中央と四隅のカメラレイが同じ非発光面に当たる画素での相対L2。明るい光源や輪郭の混合画素が支配しない指標として併記する。小さい特徴や遮蔽境界を完全に除外した指標ではない。
- `summary.csv`: ランナーが各実行の指標を集約する。

参照画像も有限サンプルであり、誤差には参照側のノイズ、画素サンプリング、直接照明のノイズ、キャッシュの補間誤差が含まれる。低解像度の予備評価を最終的な品質保証としないこと。速度比較では表面方式の照明計算と描画の両方を含める。ただしサンプル数と品質が異なるため、時間比を「同品質での高速化率」と呼ばない。

## 現在の限界

表面粒子方式では全表面粒子を照明計算のプローブとして使用しており、少数プローブから多数粒子への補間は今後の段階。一般三角形シーン、GPU、同品質でのフォトンマッピング比較、適応配置、時間蓄積、鏡面・屈折・コースティクスは未実装。

各面の格子解像度は同じなので、小さい面は面積あたりの粒子密度が高い。面内の遮蔽境界でも補間が陰影をぼかす可能性があり、同一面制限だけで全ての光漏れがなくなるわけではない。小サンプル数では空間的なむらが残る。

`--check` は遮蔽、余弦分布、同一面の補間、反射率ゼロ、光源ゼロの不変条件を確認する。これらは収束や物理精度の証明ではない。

次の段階は高解像度・複数乱数種での誤差対時間の比較、プローブの間引き、独立した既存レンダラとの比較である。

初回の記録は [研究結果](../../../reaserch/cornell_surface_transport/results/2026-10-07/README.md) を参照。表示用PNGは `preview.py`（任意のPillow依存）でPPMから作成できる。

## ライブラリのフォトンマッピングを実行する

別方式として `RayTracerCore` に追加した `PhotonMapper` も同じ実験シーンで実行できる。

```powershell
.\Phantom\build\windows-release\RayTracer\CornellSurfaceTransport.exe --photon scratch/cornell-photon 96 200000 0.12 32 42
```

詳細は [PhotonMapperのAPIと実行手順](../doc/photon_mapping.md) を参照。従来の表面粒子方式とは、推定方法・反射深度・サンプル条件が異なるため、単純な実行時間比を同品質での性能比としない。
