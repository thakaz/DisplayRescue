#pragma once

#include <DirectXMath.h>

#include <cstddef>
#include <optional>
#include <vector>

namespace rescue::mascot {
	// ノード 1 つの置き方（移動・回転・拡大）。回転はクォータニオン（x, y, z, w）。glTF と同じ持ち方
	struct NodeTransform {
		DirectX::XMFLOAT3 translation{ 0, 0, 0 };
		DirectX::XMFLOAT4 rotation{ 0, 0, 0, 1 };
		DirectX::XMFLOAT3 scale{ 1, 1, 1 };
	};

	// 置き方を行列にする（拡大 → 回転 → 移動の順に掛ける）
	DirectX::XMMATRIX ToMatrix(const NodeTransform& t);

	enum class AnimationPath { Translation, Rotation, Scale };
	enum class Interpolation { Linear, Step, CubicSpline };

	// アニメーションの 1 本: あるノードの、移動か回転か拡大のどれかを、時刻ごとに書いたもの
	struct AnimationChannel {
		std::size_t node = 0;
		AnimationPath path = AnimationPath::Translation;
		Interpolation interpolation = Interpolation::Linear;
		std::vector<float> times;					// キーフレームの時刻（秒、小さい順）
		std::vector<DirectX::XMFLOAT4> values;		// キーフレームの値（移動・拡大は xyz だけ使う）。CubicSpline は 1 つの時刻に 3 つ（入りの傾き・値・出の傾き）
	};

	// 骨の行列 1 つ分の作り方。頂点の joints はこの配列（パレット）の番号を指す
	struct PaletteEntry {
		std::size_t node = 0;					// どのノード（骨）の動きについていくか
		DirectX::XMFLOAT4X4 inverseBind;		// 逆バインド行列。骨の付いていないメッシュは単位行列
	};

	// 骨とアニメーション。glTF から読んだもの（GltfMesh.cpp が作る）を持ち、時刻ごとの骨の行列を計算する。
	// 何も覚えない（const）。揺れは前のフレームを覚える必要があるので、別のクラス（SpringSimulator）が(2)と(3)の間に割り込む
	struct Skeleton {
		std::vector<int> parents;					// ノードごとの親の番号（-1 は親なし）
		std::vector<std::size_t> order;				// 親が子より先に来る順のノード番号（親の行列を先に計算するため）
		std::vector<NodeTransform> rest;			// ノードごとの基本の置き方（アニメーションが触らないところはこのまま）
		std::vector<AnimationChannel> channels;		// 1 つ目のアニメーション
		float duration = 0.0f;						// アニメーションの長さ（秒）
		std::vector<PaletteEntry> palette;
		DirectX::XMFLOAT4X4 normalize;				// 最後に掛ける行列: Z の反転（右手系 → 左手系）と、-1〜1 にそろえる移動・拡大

		// 1. ノードごとの置き方（親から見た置き方）。seconds が無ければ基本の姿勢（アニメーションなし）。
		// アニメーションは duration で繰り返す
		std::vector<NodeTransform> LocalTransforms(std::optional<float> seconds) const;

		// 2. ノードごとの世界の置き方（glTF の座標。normalize はまだ掛けない）。親から順に掛ける
		std::vector<DirectX::XMFLOAT4X4> WorldTransforms(const std::vector<NodeTransform>& local) const;

		// 3. パレットの行列（頂点に掛ける行列）。逆バインド × 骨の世界の置き方 × normalize
		std::vector<DirectX::XMFLOAT4X4> Palette(const std::vector<DirectX::XMFLOAT4X4>& world) const;

		// (1)〜(3)をまとめて。行列は DirectXMath の並び（転置していない）
		std::vector<DirectX::XMFLOAT4X4> JointMatrices(std::optional<float> seconds) const {
			return Palette(WorldTransforms(LocalTransforms(seconds)));
		}
	};
}
