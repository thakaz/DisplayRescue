#pragma once

#include <DirectXMath.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "mascot/Humanoid.h"
#include "mascot/Skeleton.h"

namespace rescue::mascot {
	// VRMA の表情 1 つ。VRMA では、表情ごとに 1 つのノードを置き、
	// その「移動の x」を効き具合（0〜1）としてアニメーションさせる（VRMC_vrm_animation の expressions）
	struct MotionExpression {
		std::wstring name;		// 表情の名前（blink / happy などの preset の名前か、custom の名前。モデルの表情と名前で突き合わせる）
		std::size_t node = 0;	// 効き具合を書いたノード（skeleton の番号）
	};

	// .vrma（中身は glTF）から読んだもの。骨の木とアニメーション（Skeleton）と、人型の骨の名前
	struct MotionData {
		Skeleton skeleton;
		HumanBones humanBones;
		std::vector<MotionExpression> expressions;	// 無ければ空（体だけのモーション）
	};

	// モーション 1 本を、モデルの骨に移して再生する（リターゲット）。
	// モーションとモデルは骨の数も並びも基本の姿勢も違うので、「人型の骨の名前」で対応を取り、
	// 骨ごとに「基本の姿勢から世界の向きでどれだけ回ったか」を移す。
	// Model が持ち、Draw のたびに Apply を呼ぶ（Skeleton の(1) LocalTransforms の代わり）
	class MotionPlayer {
	public:
		// motion: 読んだモーション。model / modelBones: モデルの骨と人型の骨（Model の m_skeleton と MeshData::humanBones）。
		// modelExpressions: モデルの表情の名前（Model の m_expressions の並び）。モーションの表情と名前で突き合わせる
		MotionPlayer(MotionData motion, const Skeleton& model, const HumanBones& modelBones, const std::vector<std::wstring>& modelExpressions);

		float Duration() const noexcept { return m_motion.duration; }
		std::size_t BoneCount() const noexcept { return m_pairs.size(); }	// 対応が取れた骨の数

		// seconds の時刻の姿勢を、モデルのノードの置き方（local。Skeleton::LocalTransforms(nullopt) の結果）に書き込む。
		// 人型の骨は回転を、hips はそれに加えて位置を書き換える。それ以外のノード（髪・スカートなど）はそのまま
		void Apply(float seconds, const Skeleton& model, std::vector<NodeTransform>& local) const;

		// seconds 時点の表情に amount（クロスフェードの重み 0〜1）を掛けて加算する。並びはモデルの表情と同じ。
		// weights には「効き具合 × amount」、control には「このモーションがその表情を動かしている度合い（amount）」を足す。
		// Model は、表情ごとに「自分で決めた効き具合（まばたきなど）」と weights を control で混ぜる
		void AddExpressions(float seconds, float amount, std::vector<float>& weights, std::vector<float>& control) const;

	private:
		// 対応の取れた骨 1 つ。rest は基本の姿勢での世界の向き（クォータニオン）
		struct BonePair {
			std::size_t motionNode = 0;
			std::size_t modelNode = 0;
			DirectX::XMFLOAT4 motionRest{ 0, 0, 0, 1 };
			DirectX::XMFLOAT4 modelRest{ 0, 0, 0, 1 };
		};

		Skeleton m_motion;
		std::vector<BonePair> m_pairs;
		std::vector<int> m_pairOfModelNode;		// モデルのノード → m_pairs の番号（-1 は人型の骨でない）
		std::optional<BonePair> m_hips;			// hips の位置を移すため
		float m_hipsScale = 1.0f;				// モデルの腰の高さ ÷ モーションの腰の高さ（背の違いで足が浮く・沈むのを防ぐ）

		// モデルにもある表情だけ。modelExpression はモデルの表情の番号、motionNode は効き具合を書いたノード
		struct ExpressionPair {
			std::size_t modelExpression = 0;
			std::size_t motionNode = 0;
		};
		std::vector<ExpressionPair> m_expressions;
	};

	// 2 つの姿勢を混ぜる。to を「from から to へ weight（0〜1）だけ進んだ姿勢」に書き換える。
	// weight が 0 なら from そのもの、1 なら to のまま。前のモーションから次のモーションへ、なめらかに移るのに使う（Model::Draw）
	void BlendPose(const std::vector<NodeTransform>& from, std::vector<NodeTransform>& to, float weight);
}
