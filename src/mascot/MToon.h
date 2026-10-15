#pragma once

#include <DirectXMath.h>

#include <cstddef>
#include <optional>
#include <string_view>

namespace rescue::mascot {
	// 輪郭線の太さの決め方（MToon の outlineWidthMode）
	enum class OutlineWidthMode {
		None,				// 輪郭線なし
		WorldCoordinates,	// 太さはモデルの座標（メートル）。遠くから見ると細くなる
		ScreenCoordinates,	// 太さは画面の高さに対する割合（このプログラムでは WorldCoordinates と同じに扱う）
	};

	// VRM の材質の拡張 VRMC_materials_mtoon（アニメ調の描き方）の値。
	// 既定値は MToon 1.0 の仕様のもの（JSON に書いていない項目はこの値になる）。
	// テクスチャは「glTF の textures の何番目か」（画像の番号ではない。画像への置き換えは GltfMesh.cpp がする）
	struct MToonMaterial {
		// 影: 明るい側は基本色、影の側はこの色。境目は shadingShift でずらし、shadingToony でくっきりさせる
		DirectX::XMFLOAT3 shadeColor{ 0, 0, 0 };			// shadeColorFactor
		std::optional<std::size_t> shadeMultiplyTexture;	// 影の色に掛けるテクスチャ
		float shadingShift = 0.0f;							// shadingShiftFactor（-1〜1。正なら影が減る）
		std::optional<std::size_t> shadingShiftTexture;		// 影の境目をずらすテクスチャ（R を使う）
		float shadingShiftTextureScale = 1.0f;				// shadingShiftTexture.scale
		float shadingToony = 0.9f;							// shadingToonyFactor（0 でなだらか、1 で 2 段）
		float giEqualization = 0.9f;						// giEqualizationFactor（このプログラムでは使わない）

		// 縁の光（リムライト）: マットキャップ（法線の向きで引く画像）と、縁ほど明るくなる光
		DirectX::XMFLOAT3 matcapColor{ 1, 1, 1 };			// matcapFactor
		std::optional<std::size_t> matcapTexture;
		DirectX::XMFLOAT3 rimColor{ 0, 0, 0 };				// parametricRimColorFactor
		float rimFresnelPower = 5.0f;						// parametricRimFresnelPowerFactor（大きいほど縁に寄る）
		float rimLift = 0.0f;								// parametricRimLiftFactor
		std::optional<std::size_t> rimMultiplyTexture;
		float rimLightingMix = 1.0f;						// rimLightingMixFactor（このプログラムでは使わない）

		// 輪郭線
		OutlineWidthMode outlineWidthMode = OutlineWidthMode::None;
		float outlineWidth = 0.0f;							// outlineWidthFactor
		std::optional<std::size_t> outlineWidthMultiplyTexture;	// 太さに掛けるテクスチャ（G を使う）
		DirectX::XMFLOAT3 outlineColor{ 0, 0, 0 };			// outlineColorFactor
		float outlineLightingMix = 1.0f;					// outlineLightingMixFactor（このプログラムでは使わない）

		// 半透明どうしの描く順番（小さいほど先）
		int renderQueueOffset = 0;							// renderQueueOffsetNumber
	};

	// VRMC_materials_mtoon の JSON（cgltf が文字列のまま渡してくるもの。UTF-8）を読む。
	// Windows.Data.Json（C++/WinRT）を使う。JSON として読めなければ winrt::hresult_error（呼ぶ側の GltfMesh.cpp が HRESULT にする）
	MToonMaterial ParseMToon(std::string_view json);
}
