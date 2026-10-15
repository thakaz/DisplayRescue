# 同梱のマスコット

設定アプリで「インストール」したとき、マスコットのモデルとモーションがまだ何も入っていなければ、このフォルダのものを入れる。

| フォルダ | 中身 |
|---|---|
| `model` | モデル(Shibu.vrm) |
| `idle` | 待機モーション(idle.vrma) |
| `motions` | 一定間隔で再生するモーション(look-around / doze) |

## モデル

`Shibu.vrm`は、VRoidの公式サンプルモデル「β Ver AvatarSample_1」(千駄ヶ谷 渋)のダークネスバージョン(Darkness_Shibu、作者coati)。

* ライセンスはCC0(pixiv Inc.が著作権と関連する権利を放棄している)
* 出典: [VRoidヘルプ「β Ver AvatarSample_1」](https://vroid.pixiv.help/hc/ja/articles/360012381793)
* VRMファイルの利用には、VRoid Hubの利用規約への同意が必要とされている

## モーション

待機モーションは、CMU Graphics Lab Motion Capture Database(http://mocap.cs.cmu.edu/)のデータを、VRMアニメーション(.vrma)に変換したもの。

| ファイル | 元の試技 |
|---|---|
| `idle/idle.vrma` | 77_02 standing |

The data used in this project was obtained from mocap.cs.cmu.edu.
The database was created with funding from NSF EIA-0196217.

`motions`の2本は、DisplayRescueのために作ったもの(ライセンスはDisplayRescueと同じMIT)。表情(まばたき・驚き・ゆるみ)も入っている。

| ファイル | 内容 |
|---|---|
| `motions/look-around.vrma` | 左右をきょろきょろ見る |
| `motions/doze.vrma` | うとうとして、はっと起きる |
