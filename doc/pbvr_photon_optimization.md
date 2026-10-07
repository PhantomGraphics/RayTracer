# PBVRフォトン輸送の最適化

到着点の間引きと再放出を繰り返す [PBVR輸送](pbvr_photon_transport.md) に、GPU集約向けの保存設定と重要度・空間層化抽出を追加した。

```cpp
settings.buildPhotonIndex = false; // GPU集約ではKDTreeを構築しない
settings.storeDirectPhotons = false; // 明示的な直接照明を使う場合
settings.pbvr.selection = PbvrSelection::SpatialPower;
settings.pbvr.uniformMix = 0.05;
settings.pbvr.spatialCellSize = 0.12; // 0ならgatherRadius
```

既定値は索引構築・直接光保存とも有効、選択方式は `Uniform`。既存のCPU照会と一様間引きの動作を維持する。索引を省いたマップのCPU密度推定は全点走査になるため、大きなマップではGPU集約に使用する。直接光の保存省略は光路追跡や乱数列を変えず、間接光・コースティクスの点を維持する。

## 抽出方式

- `Uniform`: 従来の独立Bernoulli間引きと、上限超過時の一様非復元抽出。
- `Power`: 反射後の輝度を重要度とする、層化した逆CDF抽出。
- `SpatialPower`: 面IDと三次元セルで層を作り、層内をパワーに応じて抽出。

新方式の再放出数は候補数Mに対して `K = ceil(M * retention)` とし、非ゼロの `maxParticles` で制限する。候補が空なら再放出しない。K=Mなら再標本化せず、従来の各粒子の乱数状態を維持する。

パワー抽出の確率は輝度比例に `uniformMix` の一様成分を混ぜる。選択確率qの点の反射後RGB光束Wを `W / (K*q)` に補正する。位置の移動や近接点の平均化は行わない。同じ点を複数回選んだ場合も、子粒子ごとに独立した方向乱数を与える。

空間方式では、Kが非空層数G以上なら各層に最低1粒子を配り、残りの予算を層の輝度で配分する。層の割当数K_gと層内確率q_localを使い `W / (K_g*q_local)` に補正する。K<Gでは全層の被覆は保証できず、層選択確率q_groupも含め `W / (K*q_group*q_local)` に補正する。どちらもRGB光束の期待値を保持する。統計の `spatialStrata` と `sampledStrata` で被覆を確認できる。

## 検証と比較

CPUテスト39件、GPUテスト8件が成功。索引の有無による密度推定、直接光保存の省略、抽出の再現性、面別被覆、RGB光束の期待値、複製粒子の乱数、鏡面経路を検証した。GPUテストはスキップ0・Vulkan検証エラー0。

[同品質の測定結果と画像](../../../reaserch/pbvr_photon_transport/results/2026-10-07/optimized_quality/README.md) を参照。今回の拡散コーネルボックスでは一様抽出がPBVRの最速設定であり、重要度・空間抽出は最適化した従来方式を上回らなかった。そのため新方式を既定にはしていない。

実装したのは索引・保存の省略、パワー抽出、空間層化の3項目。次数別の適応予算とGPUでのフォトン輸送は未実装。輸送はCPU、最終集約はGPUのままである。
