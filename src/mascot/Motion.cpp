// モーションをモデルの骨に移す（リターゲット）。毎フレーム Model::Draw から呼ばれる。
//
// なぜ回転をそのまま写せないか: 骨の回転（local）は「親から見た、基本の姿勢からの回り方」ではなく「親から見た向きそのもの」。
// 基本の姿勢（T ポーズ）が同じでも、骨の軸の取り方は作り方しだいで違う（基本の姿勢で左の太ももが
// Y まわりに 180 度回っているモーションや、首が X まわりに 26 度回っているモデルがある）。そこで、骨ごとに
//   「モーションの骨が、基本の姿勢から世界の向きでどれだけ回ったか」を求め、
//   「モデルの骨を、モデルの基本の姿勢から同じだけ回す」。
// 世界の向きで比べるのは、どちらも T ポーズで同じ方向を向いている（人型の骨の決まり）から。
// 最後の BlendPose は、2 つの姿勢を混ぜる（モーションの切り替えをなめらかにする）
// AddExpressions: VRMA の表情（効き具合を書いたノード）を、名前で突き合わせたモデルの表情に移す
#include "mascot/Motion.h"

#include <algorithm>

namespace {
	using namespace DirectX;

	XMVECTOR RotationOf(const XMFLOAT4X4& m) {
		XMVECTOR scale, rotation, translation;
		XMMatrixDecompose(&scale, &rotation, &translation, XMLoadFloat4x4(&m));
		return rotation;
	}

	XMVECTOR PositionOf(const XMFLOAT4X4& m) {
		return XMVector3TransformCoord(XMVectorZero(), XMLoadFloat4x4(&m));
	}
}

namespace rescue::mascot {

	MotionPlayer::MotionPlayer(MotionData motion, const Skeleton& model, const HumanBones& modelBones, const std::vector<std::wstring>& modelExpressions)
		: m_motion(std::move(motion.skeleton)), m_pairOfModelNode(model.parents.size(), -1) {
		// 0. 名前で突き合わせる。モデルに無い表情は捨てる
		for (const MotionExpression& expression : motion.expressions) {
			const auto found = std::ranges::find(modelExpressions, expression.name);
			if (found == modelExpressions.end() || expression.node >= m_motion.parents.size()) continue;
			m_expressions.push_back({ static_cast<std::size_t>(found - modelExpressions.begin()), expression.node });
		}

		// 基本の姿勢での世界の置き方（どちらもアニメーションなし）
		const std::vector<XMFLOAT4X4> motionRest = m_motion.WorldTransforms(m_motion.LocalTransforms(std::nullopt));
		const std::vector<XMFLOAT4X4> modelRest = model.WorldTransforms(model.LocalTransforms(std::nullopt));

		// 1. 名前で突き合わせる。モーションの骨を 1 つずつ見て、モデルにも同じ名前があれば組にする
		for (const auto& [name, motionNode] : motion.humanBones) {
			const auto found = modelBones.find(name);
			if (found == modelBones.end()) continue;
			const std::size_t modelNode = found->second;
			if (motionNode >= motionRest.size() || modelNode >= modelRest.size()) continue;

			BonePair pair{ motionNode, modelNode };
			XMStoreFloat4(&pair.motionRest, RotationOf(motionRest[motionNode]));
			XMStoreFloat4(&pair.modelRest, RotationOf(modelRest[modelNode]));
			m_pairOfModelNode[modelNode] = static_cast<int>(m_pairs.size());
			m_pairs.push_back(pair);
			if (name == L"hips") m_hips = pair;
		}

		// 2. 腰の高さの比。モーションを作った人とモデルの背が違っても、足が床から浮いたり沈んだりしないように、腰の位置を縮める
		if (m_hips) {
			const float motionHeight = XMVectorGetY(PositionOf(motionRest[m_hips->motionNode]));
			const float modelHeight = XMVectorGetY(PositionOf(modelRest[m_hips->modelNode]));
			if (motionHeight > 1e-4f) m_hipsScale = modelHeight / motionHeight;
		}
	}

	void MotionPlayer::Apply(float seconds, const Skeleton& model, std::vector<NodeTransform>& local) const {
		// モーション側: この時刻の世界の置き方（Skeleton の(1)(2)をモーションの骨で回す）
		const std::vector<XMFLOAT4X4> motionWorld = m_motion.WorldTransforms(m_motion.LocalTransforms(seconds));

		// モデル側: 親から順に。人型の骨は、親の「今の」世界の向きが要るので、世界の置き方を計算しながら進む
		std::vector<XMFLOAT4X4> world(local.size());
		for (const std::size_t n : model.order) {
			const int parent = model.parents[n];
			const XMMATRIX parentWorld = parent < 0 ? XMMatrixIdentity() : XMLoadFloat4x4(&world[parent]);

			if (const int p = m_pairOfModelNode[n]; p >= 0) {
				const BonePair& pair = m_pairs[static_cast<std::size_t>(p)];
				const XMVECTOR motionWorldRotation = RotationOf(motionWorld[pair.motionNode]);
				// XMQuaternionMultiply(q1, q2) は「q1 で回してから q2 で回す」。Conjugate（共役）は単位クォータニオンの逆回転

				// モーションの骨が、基本の姿勢から世界の向きでどれだけ回ったか。
				// 「基本の姿勢の向き（motionRest）を戻してから、今の向き（motionWorldRotation）に回す」
				const XMVECTOR delta = XMQuaternionMultiply(XMQuaternionConjugate(XMLoadFloat4(&pair.motionRest)), motionWorldRotation);
				// モデルの骨の、今の世界の向き。「モデルの基本の姿勢の向き（modelRest）から、delta だけ回す」
				const XMVECTOR modelWorldRotation = XMQuaternionMultiply(XMLoadFloat4(&pair.modelRest), delta);

				// 世界の向き → 親から見た向き（local）: 「世界の向きにしてから、親の向きを戻す」
				const XMVECTOR parentRotation = parent < 0 ? XMQuaternionIdentity() : RotationOf(world[parent]);
				XMStoreFloat4(&local[n].rotation, XMQuaternionNormalize(XMQuaternionMultiply(modelWorldRotation, XMQuaternionConjugate(parentRotation))));

				// hips は位置も。モーションの腰の位置を背の比で縮め、親から見た位置に直す
				if (m_hips && n == m_hips->modelNode) {
					const XMVECTOR target = PositionOf(motionWorld[pair.motionNode]) * m_hipsScale;
					XMStoreFloat3(&local[n].translation, XMVector3TransformCoord(target, XMMatrixInverse(nullptr, parentWorld)));
				}
			}
			XMStoreFloat4x4(&world[n], ToMatrix(local[n]) * parentWorld);
		}
	}

	// 効き具合は、表情のノードの「移動の x」。0〜1 に収める（VRMA の決まり）
	void MotionPlayer::AddExpressions(float seconds, float amount, std::vector<float>& weights, std::vector<float>& control) const {
		if (m_expressions.empty() || amount <= 0.0f) return;
		const std::vector<NodeTransform> local = m_motion.LocalTransforms(seconds);
		for (const ExpressionPair& pair : m_expressions) {
			if (pair.modelExpression >= weights.size()) continue;
			weights[pair.modelExpression] += amount * std::clamp(local[pair.motionNode].translation.x, 0.0f, 1.0f);
			control[pair.modelExpression] += amount;
		}
	}

	// ノードごとに、位置は Lerp（まっすぐ混ぜる）、回転は Slerp（球の上を混ぜる）。Skeleton.cpp の Sample と同じ混ぜ方
	void BlendPose(const std::vector<NodeTransform>& from, std::vector<NodeTransform>& to, float weight) {
		for (std::size_t n = 0; n < to.size() && n < from.size(); ++n) {
			const NodeTransform& a = from[n];
			NodeTransform& b = to[n];
			XMStoreFloat3(&b.translation, XMVectorLerp(XMLoadFloat3(&a.translation), XMLoadFloat3(&b.translation), weight));
			// 回転を混ぜる。from の回転（a.rotation）から to の回転（b.rotation）へ、weight だけ進んだ回転
			const XMVECTOR rotation = XMQuaternionSlerp(XMLoadFloat4(&a.rotation), XMLoadFloat4(&b.rotation), weight);
			XMStoreFloat4(&b.rotation, rotation);
			XMStoreFloat3(&b.scale, XMVectorLerp(XMLoadFloat3(&a.scale), XMLoadFloat3(&b.scale), weight));
		}
	}
}
