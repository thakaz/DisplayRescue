#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace rescue::mascot {
	// 表情 1 つが動かすモーフ 1 つ。「node 番目のノードが持つメッシュの、index 番目のモーフを、weight の割合で」
	struct MorphTargetBind {
		std::size_t node = 0;		// glTF の nodes の番号
		std::size_t index = 0;		// そのメッシュのモーフ（targets）の番号
		float weight = 1.0f;		// 表情を 100% にしたときの、このモーフの効き具合（0〜1）
	};

	// VRM の表情 1 つ（VRMC_vrm の expressions の preset / custom の 1 つ）
	struct VrmExpression {
		std::wstring name;			// preset はその名前（blink / happy / aa など）、custom は作者が付けた名前
		bool isBinary = false;		// true なら 0 か 1 のどちらかにする（0.5 より大きければ 1）
		std::vector<MorphTargetBind> binds;
	};

	// VRMC_vrm の JSON（cgltf が文字列のまま渡してくるもの。UTF-8）から、表情を全部読む。
	// Windows.Data.Json（C++/WinRT）を使う。JSON として読めなければ winrt::hresult_error（呼ぶ側の GltfMesh.cpp が HRESULT にする）
	std::vector<VrmExpression> ParseExpressions(std::string_view json);
}
