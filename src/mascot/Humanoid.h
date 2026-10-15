#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace rescue::mascot {
	// 人型の骨の名前（hips / spine / leftUpperArm など。VRM の仕様で決まっている）→ glTF の nodes の番号。
	// モデル（.vrm の VRMC_vrm）とモーション（.vrma の VRMC_vrm_animation）の両方に同じ形で書いてあるので、名前で突き合わせれば
	// 「モーションのこの骨の動きを、モデルのこの骨に」が決まる
	using HumanBones = std::unordered_map<std::wstring, std::size_t>;

	// 拡張の JSON（VRMC_vrm か VRMC_vrm_animation。cgltf が文字列のまま渡してくるもの。UTF-8）から、
	// humanoid.humanBones を読む。形は { "humanoid": { "humanBones": { "hips": { "node": 3 }, "spine": { "node": 4 }, ... } } }。
	// humanoid が無ければ空。Windows.Data.Json（C++/WinRT）を使う。JSON として読めなければ winrt::hresult_error
	HumanBones ParseHumanBones(std::string_view json);
}
