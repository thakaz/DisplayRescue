#pragma once

#include <DirectXMath.h>

#include <cstddef>
#include <vector>

#include "mascot/Skeleton.h"
#include "mascot/SpringBone.h"

namespace rescue::mascot {
	// VRM の揺れもの（SpringBone）を毎フレーム動かす。
	// Skeleton は時刻から姿勢を決めるだけで何も覚えないが、揺れは「前のフレームのテール（骨の先端）」を覚えておく必要がある。
	// その状態をこのクラスが持つ。Model が 1 つ持ち、Draw のたびに Update を呼ぶ（Skeleton の(2)と(3)の間）
	class SpringSimulator {
	public:
		SpringSimulator() = default;	// 揺れもの無し（VRM でないモデル）

		// data: 読み込んだ揺れもの。nodeCount: glTF のノードの数（範囲外の番号を指すジョイントやコライダーは捨てる）
		SpringSimulator(const SpringBoneData& data, std::size_t nodeCount);

		bool Empty() const noexcept { return m_chains.empty(); }

		// 揺れる骨の回転を local に書き込み、world（Skeleton::WorldTransforms の結果）を計算し直す。
		// modelToScene: glTF の座標から画面に置いた座標への行列（normalize × Draw の world）。モデルが回ると、ここから「体が動いた」が伝わる。
		// deltaSeconds: 前のフレームからの時間。長すぎると暴れるので中で 1/30 秒に切る
		void Update(const Skeleton& skeleton, std::vector<NodeTransform>& local, std::vector<DirectX::XMFLOAT4X4>& world,
			DirectX::FXMMATRIX modelToScene, float deltaSeconds);

	private:
		// 揺れる骨 1 本。node の骨を、tailNode の位置（テール）が「揺れたテール」に来るように回す
		struct Bone {
			std::size_t node = 0;
			std::size_t tailNode = 0;
			SpringJoint settings;
			DirectX::XMFLOAT3 axis{ 0, 1, 0 };	// 基本の姿勢での、骨から見たテールの向き（長さ 1）。最初の Update で決める
			float length = 0.0f;				// 骨の長さ（揺らす空間で）。最初の Update で決める
			DirectX::XMFLOAT3 previousTail{};	// 前のフレームのテール（揺らす空間で）
			DirectX::XMFLOAT3 currentTail{};	// 今のテール（揺らす空間で）
		};

		// スプリング 1 本分。骨は根元から先端の順
		struct Chain {
			std::vector<Bone> bones;
			std::vector<std::size_t> colliders;	// 当たるコライダー（m_colliders の番号）。グループを開いて並べたもの
		};

		std::vector<SpringCollider> m_colliders;
		std::vector<Chain> m_chains;
		bool m_initialized = false;	// 最初の Update でテールの位置・骨の長さを決めたか
	};
}
