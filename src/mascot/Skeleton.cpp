// 骨のアニメーションの計算。毎フレーム Model::Draw から呼ばれる。
// 流れ: (1)時刻からキーフレームを探して値を混ぜる（FindKeyframe は Keyframe.cpp）→ (2)親から順に行列を掛けてノードの世界の置き方 → (3)パレット
// (1)(2)(3)は別々の関数。Model::Draw が(1)の後にモーションを、(2)と(3)の間に揺れ（SpringSimulator）を挟むため
#include "mascot/Skeleton.h"

#include <algorithm>
#include <cmath>

#include "mascot/Keyframe.h"

namespace {
	using namespace DirectX;
	using rescue::mascot::AnimationChannel;
	using rescue::mascot::AnimationPath;
	using rescue::mascot::Interpolation;

	// 1 本のアニメーションの、time での値。
	// LINEAR は前後のキーフレームを混ぜる（回転は Slerp: 球の上をまっすぐ進むように混ぜる。ただの Lerp だと長さが縮んで形が崩れる）。
	// STEP は前のキーフレームのまま。CUBICSPLINE は本来は傾きを使った曲線だが、ここでは値だけを LINEAR で混ぜる（近似）
	XMVECTOR Sample(const AnimationChannel& channel, float time) {
		const auto [index, fraction] = rescue::mascot::FindKeyframe(channel.times, time);
		const std::size_t next = (std::min)(index + 1, channel.times.size() - 1);

		const bool cubic = channel.interpolation == Interpolation::CubicSpline;
		const auto value = [&](std::size_t k) { return XMLoadFloat4(&channel.values[cubic ? k * 3 + 1 : k]); };
		const XMVECTOR a = value(index);
		if (channel.interpolation == Interpolation::Step) return a;

		const XMVECTOR b = value(next);
		return channel.path == AnimationPath::Rotation ? XMQuaternionSlerp(a, b, fraction) : XMVectorLerp(a, b, fraction);
	}
}

namespace rescue::mascot {

	XMMATRIX ToMatrix(const NodeTransform& t) {
		return XMMatrixScalingFromVector(XMLoadFloat3(&t.scale))
			* XMMatrixRotationQuaternion(XMLoadFloat4(&t.rotation))
			* XMMatrixTranslationFromVector(XMLoadFloat3(&t.translation));
	}

	// 1. アニメーションが触るところだけ、基本の置き方を上書きする
	std::vector<NodeTransform> Skeleton::LocalTransforms(std::optional<float> seconds) const {
		std::vector<NodeTransform> local = rest;
		if (seconds && duration > 0.0f) {
			// 長さで割った余り = 繰り返し。ただし長さちょうどまでは余りを取らない。
			// 余りを取ると、長さちょうど（= 最後の姿勢）が 0（= 最初の姿勢）になる。クロスフェードで「終了したモーションの最後の姿勢」を
			// Apply(長さ) で取り出すとき、最初の姿勢が出てきて、モーションの終わりでガクッと跳んでしまう
			const float time = *seconds >= 0.0f && *seconds <= duration ? *seconds : std::fmod(*seconds, duration);
			for (const AnimationChannel& channel : channels) {
				const XMVECTOR v = Sample(channel, time);
				NodeTransform& target = local[channel.node];
				switch (channel.path) {
				case AnimationPath::Translation: XMStoreFloat3(&target.translation, v); break;
				case AnimationPath::Rotation:    XMStoreFloat4(&target.rotation, XMQuaternionNormalize(v)); break;
				case AnimationPath::Scale:       XMStoreFloat3(&target.scale, v); break;
				}
			}
		}
		return local;
	}

	// 2. ノードの世界の置き方。自分の置き方 × 親の世界の置き方。
	// order は親が先に来るので、親の分はもう計算してある
	std::vector<XMFLOAT4X4> Skeleton::WorldTransforms(const std::vector<NodeTransform>& local) const {
		std::vector<XMFLOAT4X4> world(local.size());
		for (const std::size_t n : order) {
			const XMMATRIX own = ToMatrix(local[n]);
			XMStoreFloat4x4(&world[n], parents[n] < 0 ? own : own * XMLoadFloat4x4(&world[parents[n]]));
		}
		return world;
	}

	// 3. パレット: 頂点に掛ける行列。最後に normalize（Z の反転と大きさをそろえる）を掛ける
	std::vector<XMFLOAT4X4> Skeleton::Palette(const std::vector<XMFLOAT4X4>& world) const {
		const XMMATRIX last = XMLoadFloat4x4(&normalize);
		std::vector<XMFLOAT4X4> matrices(palette.size());
		for (std::size_t k = 0; k < palette.size(); ++k) {
			const XMMATRIX inverseBind = XMLoadFloat4x4(&palette[k].inverseBind);
			// 骨の行列。頂点は「基本の姿勢でのモデルの座標」で書かれているので、
			// 骨の動きを掛ける前に、逆バインド行列（inverseBind）で「骨から見た座標」に直す
			const XMMATRIX joint = inverseBind * XMLoadFloat4x4(&world[palette[k].node]);
			XMStoreFloat4x4(&matrices[k], joint * last);
		}
		return matrices;
	}
}
