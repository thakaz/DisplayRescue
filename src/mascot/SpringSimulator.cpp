// 揺れもの（SpringBone）の計算。毎フレーム Model::Draw から呼ばれる。
// 計算は VRM の仕様（VRMC_springBone-1.0）のとおり。
//
// 空間が 2 つあるので注意:
// * glTF の座標: Skeleton が計算する world（ノードの世界の置き方）。骨の回転（クォータニオン）はこちらで計算する
// * 揺らす空間: 画面に置いた座標（normalize × Draw の world）から、大きさだけを元（メートル）に戻したもの。
//   テールの位置・重力・当たり判定はこちら。モデルが回る（配置の調整で回したときなど）と、ここでは体が動いたように見えるので、髪が遅れてついてくる。
//   normalize には Z の反転（鏡写し）が入っているので、クォータニオンはこちらでは扱わない（向きだけ glTF の座標に戻して回転を作る）
#include "mascot/SpringSimulator.h"

#include <algorithm>
#include <cmath>
#include <variant>

namespace {
	using namespace DirectX;
	using rescue::mascot::CapsuleShape;
	using rescue::mascot::SphereShape;
	using rescue::mascot::SpringCollider;

	constexpr float kMaxDeltaSeconds = 1.0f / 30.0f;	// これより長い経過時間は切る（ロック中に止まっていた後などに暴れないように）

	XMVECTOR Position(FXMMATRIX m) { return XMVector3TransformCoord(XMVectorZero(), m); }

	// from の向きを to の向きに回すクォータニオン（どちらも長さ 1）。同じ向き・真逆は回さない
	XMVECTOR FromTo(FXMVECTOR from, FXMVECTOR to) {
		const XMVECTOR axis = XMVector3Cross(from, to);
		if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-12f) return XMQuaternionIdentity();
		const float cosine = std::clamp(XMVectorGetX(XMVector3Dot(from, to)), -1.0f, 1.0f);
		return XMQuaternionRotationAxis(XMVector3Normalize(axis), std::acos(cosine));
	}

	// 当たり判定を、揺らす空間の線分（a〜b）と半径にする。球は a と b が同じ点
	struct Segment {
		XMVECTOR a;
		XMVECTOR b;
		float radius;
	};

	// std::get_if: variant に入っているのがその型ならポインタ、違えば nullptr
	Segment ToSegment(const SpringCollider& collider, FXMMATRIX nodeToSim) {
		if (const SphereShape* sphere = std::get_if<SphereShape>(&collider.shape)) {
			const XMVECTOR center = XMVector3TransformCoord(XMLoadFloat3(&sphere->offset), nodeToSim);
			return { center, center, sphere->radius };
		}
		const CapsuleShape& capsule = std::get<CapsuleShape>(collider.shape);
		return {
			XMVector3TransformCoord(XMLoadFloat3(&capsule.offset), nodeToSim),
			XMVector3TransformCoord(XMLoadFloat3(&capsule.tail), nodeToSim),
			capsule.radius,
		};
	}

	// 線分 a〜b の上で、p にいちばん近い点
	XMVECTOR ClosestPoint(FXMVECTOR a, FXMVECTOR b, FXMVECTOR p) {
		const XMVECTOR ab = b - a;
		const float lengthSq = XMVectorGetX(XMVector3LengthSq(ab));
		if (lengthSq < 1e-12f) return a;
		const float t = std::clamp(XMVectorGetX(XMVector3Dot(p - a, ab)) / lengthSq, 0.0f, 1.0f);
		return a + ab * t;
	}
}

namespace rescue::mascot {

	SpringSimulator::SpringSimulator(const SpringBoneData& data, std::size_t nodeCount) {
		// コライダー: 範囲外のノードを指すものは捨てる。番号がずれるので「元の番号 → 新しい番号」を覚えておく
		std::vector<std::ptrdiff_t> colliderIndex(data.colliders.size(), -1);
		for (std::size_t c = 0; c < data.colliders.size(); ++c) {
			if (data.colliders[c].node >= nodeCount) continue;
			colliderIndex[c] = static_cast<std::ptrdiff_t>(m_colliders.size());
			m_colliders.push_back(data.colliders[c]);
		}

		for (const Spring& spring : data.springs) {
			Chain chain;
			// ジョイント i の骨は、ジョイント i + 1 の位置をテールにする。最後のジョイントは骨にならない
			for (std::size_t i = 0; i + 1 < spring.joints.size(); ++i) {
				const SpringJoint& joint = spring.joints[i];
				const std::size_t tail = spring.joints[i + 1].node;
				if (joint.node >= nodeCount || tail >= nodeCount) break;	// 鎖が途切れたら、そこから先は揺らさない
				chain.bones.push_back({ .node = joint.node, .tailNode = tail, .settings = joint });
			}
			if (chain.bones.empty()) continue;

			// コライダーグループを開いて、コライダーの番号を並べる（重なりは除く）
			for (const std::size_t group : spring.colliderGroups) {
				if (group >= data.colliderGroups.size()) continue;
				for (const std::size_t c : data.colliderGroups[group]) {
					if (c >= colliderIndex.size() || colliderIndex[c] < 0) continue;
					const auto index = static_cast<std::size_t>(colliderIndex[c]);
					if (std::ranges::find(chain.colliders, index) == chain.colliders.end()) chain.colliders.push_back(index);
				}
			}
			m_chains.push_back(std::move(chain));
		}
	}

	void SpringSimulator::Update(const Skeleton& skeleton, std::vector<NodeTransform>& local, std::vector<XMFLOAT4X4>& world,
		FXMMATRIX modelToScene, float deltaSeconds) {
		if (m_chains.empty()) return;
		const float dt = std::clamp(deltaSeconds, 0.0f, kMaxDeltaSeconds);

		// 揺らす空間: modelToScene から大きさ（normalize の拡大）を除いたもの。大きさは 1 行目の長さ（拡大は 3 軸とも同じ）
		const float scale = XMVectorGetX(XMVector3Length(modelToScene.r[0]));
		const XMMATRIX toSim = modelToScene * XMMatrixScaling(1.0f / scale, 1.0f / scale, 1.0f / scale);
		const XMMATRIX fromSim = XMMatrixInverse(nullptr, toSim);
		const auto simPosition = [&](std::size_t node) { return Position(XMLoadFloat4x4(&world[node]) * toSim); };

		// 0. 最初の 1 回: 今の姿勢から、テールの位置・骨の長さ・骨から見たテールの向きを決める
		if (!m_initialized) {
			for (Chain& chain : m_chains) {
				for (Bone& bone : chain.bones) {
					const XMVECTOR head = simPosition(bone.node);
					const XMVECTOR tail = simPosition(bone.tailNode);
					bone.length = XMVectorGetX(XMVector3Length(tail - head));
					XMStoreFloat3(&bone.currentTail, tail);
					bone.previousTail = bone.currentTail;
					// 向きは glTF の座標で、骨の行列の逆を掛けて「骨から見た向き」にする
					const XMMATRIX boneWorld = XMLoadFloat4x4(&world[bone.node]);
					const XMVECTOR direction = Position(XMLoadFloat4x4(&world[bone.tailNode])) - Position(boneWorld);
					XMStoreFloat3(&bone.axis, XMVector3Normalize(XMVector3TransformNormal(direction, XMMatrixInverse(nullptr, boneWorld))));
				}
			}
			m_initialized = true;
		}

		for (Chain& chain : m_chains) {
			// この鎖が当たるコライダーの、今の位置（体の骨は揺れで動かないので、鎖ごとに 1 回計算すればよい）
			std::vector<Segment> segments;
			segments.reserve(chain.colliders.size());
			for (const std::size_t c : chain.colliders) {
				segments.push_back(ToSegment(m_colliders[c], XMLoadFloat4x4(&world[m_colliders[c].node]) * toSim));
			}

			// 根元から順に。根元の骨を回すと、その先の骨の位置が変わるので、骨ごとに世界の置き方を計算し直す
			for (Bone& bone : chain.bones) {
				if (bone.length <= 1e-6f) continue;	// 長さ 0 の骨は向きが決まらない
				const int parent = skeleton.parents[bone.node];
				const XMMATRIX parentWorld = parent < 0 ? XMMatrixIdentity() : XMLoadFloat4x4(&world[parent]);
				const XMMATRIX boneWorld = ToMatrix(local[bone.node]) * parentWorld;	// 揺らす前の、この骨の世界の置き方

				const XMVECTOR head = Position(boneWorld * toSim);
				const XMVECTOR restDirection = XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&bone.axis), boneWorld * toSim));	// 「元の向き」
				const XMVECTOR current = XMLoadFloat3(&bone.currentTail);
				const XMVECTOR previous = XMLoadFloat3(&bone.previousTail);
				const SpringJoint& s = bone.settings;
				// 重力の向きは画面の座標（Y が上）。揺らす空間は Z が反転しているので Z を返す
				const XMVECTOR gravityDir = XMVectorSet(s.gravityDir.x, s.gravityDir.y, -s.gravityDir.z, 0);

				const XMVECTOR inertia = (current - previous) * (1.0f - s.dragForce);	// 慣性: 前のフレームの勢いを少し弱めて持ち越す
				const XMVECTOR stiffness = restDirection * (s.stiffness * dt);			// 戻る力: 元の向きに戻ろうとする
				const XMVECTOR gravity = gravityDir * (s.gravityPower * dt);			// 重力: 下に引っぱる

				// 次のテール = 今のテール + 慣性 + 戻る力 + 重力
				XMVECTOR next = current + inertia + stiffness + gravity;
				// 骨は伸び縮みしない: 根元（head）から次のテールへの向きはそのままで、長さを bone.length にそろえる
				next = head + XMVector3Normalize(next - head) * bone.length;

				// 当たり判定: コライダーの中に入っていたら、外に押し出してから、また長さをそろえる
				for (const Segment& segment : segments) {
					const XMVECTOR closest = ClosestPoint(segment.a, segment.b, next);
					const float reach = segment.radius + s.hitRadius;
					const XMVECTOR away = next - closest;
					const float distance = XMVectorGetX(XMVector3Length(away));
					if (distance >= reach || distance <= 1e-6f) continue;
					next = closest + away / distance * reach;
					next = head + XMVector3Normalize(next - head) * bone.length;
				}

				XMStoreFloat3(&bone.previousTail, current);
				XMStoreFloat3(&bone.currentTail, next);

				// 骨を回す。glTF の座標で「元の向き → 根元から次のテールへの向き」の回転を作り、
				// 親から見た回転（local）に直す: 新しい local = 元の local → 親の回転 → 揺れの回転 → 親の回転の逆
				const XMVECTOR fromDirection = XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&bone.axis), boneWorld));
				const XMVECTOR toDirection = XMVector3Normalize(XMVector3TransformNormal(next - head, fromSim));
				XMVECTOR parentScale, parentRotation, parentTranslation;
				XMMatrixDecompose(&parentScale, &parentRotation, &parentTranslation, parentWorld);
				// XMQuaternionMultiply(q1, q2) は「q1 で回してから q2 で回す」
				XMVECTOR rotation = XMQuaternionMultiply(XMLoadFloat4(&local[bone.node].rotation), parentRotation);
				rotation = XMQuaternionMultiply(rotation, FromTo(fromDirection, toDirection));
				rotation = XMQuaternionMultiply(rotation, XMQuaternionConjugate(parentRotation));
				XMStoreFloat4(&local[bone.node].rotation, XMQuaternionNormalize(rotation));
				XMStoreFloat4x4(&world[bone.node], ToMatrix(local[bone.node]) * parentWorld);
			}
		}

		// 回した骨の先（テールのノードや、その下のノード）の世界の置き方を計算し直す
		world = skeleton.WorldTransforms(local);
	}
}
