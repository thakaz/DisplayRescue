#pragma once

#include <DirectXMath.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace rescue::mascot {
	// VRM の揺れもの（拡張 VRMC_springBone）を読んだもの。既定値は VRM の仕様のとおり

	// 当たり判定の形: 球。offset はノードから見た中心の位置
	struct SphereShape {
		DirectX::XMFLOAT3 offset{ 0, 0, 0 };
		float radius = 0.0f;
	};

	// 当たり判定の形: カプセル（offset と tail を結ぶ線分に、半径 radius の太さを付けたもの）
	struct CapsuleShape {
		DirectX::XMFLOAT3 offset{ 0, 0, 0 };
		float radius = 0.0f;
		DirectX::XMFLOAT3 tail{ 0, 0, 0 };
	};

	// 当たり判定 1 つ。node のノード（頭・胸・腕など）にくっついて動く。
	// shape は「球かカプセルのどちらか片方」を入れる箱（std::variant）
	struct SpringCollider {
		std::size_t node = 0;
		std::variant<SphereShape, CapsuleShape> shape;
	};

	// 鎖の中の骨 1 つ。最後のジョイントは「1 つ前の骨の先端（テール）の位置」を教えるためだけのもので、数値は使わない
	struct SpringJoint {
		std::size_t node = 0;
		float hitRadius = 0.0f;						// 骨の先端の当たり判定の半径
		float stiffness = 1.0f;						// 元の向きに戻ろうとする強さ
		float gravityPower = 0.0f;					// 重力の強さ
		DirectX::XMFLOAT3 gravityDir{ 0, -1, 0 };	// 重力の向き
		float dragForce = 0.5f;						// 前のフレームの勢いを弱める割合（1 なら勢いが残らない）
	};

	// 揺れる骨の鎖 1 本（髪の 1 房、スカートの 1 枚など）
	struct Spring {
		std::wstring name;
		std::vector<SpringJoint> joints;			// 根元から先端の順
		std::vector<std::size_t> colliderGroups;	// どのコライダーグループに当たるか（SpringBoneData::colliderGroups の番号）
	};

	// VRMC_springBone の中身。番号はどれも JSON に書いてあるまま（範囲の確認は SpringSimulator がする）
	struct SpringBoneData {
		std::vector<SpringCollider> colliders;
		std::vector<std::vector<std::size_t>> colliderGroups;	// グループごとの、colliders の番号の並び
		std::vector<Spring> springs;
	};

	// VRMC_springBone の JSON（cgltf が文字列のまま渡してくるもの。UTF-8）を読む。
	// Windows.Data.Json（C++/WinRT）を使う。JSON として読めなければ winrt::hresult_error（呼ぶ側の GltfMesh.cpp が HRESULT にする）
	SpringBoneData ParseSpringBones(std::string_view json);
}
