#include "mascot/MToon.h"

#include <string>

#include "mascot/JsonRead.h"	// C++/WinRT の JSON（Windows.Data.Json）と、数・3 つ組を読む関数

#pragma comment(lib, "windowsapp.lib")

namespace {
	using winrt::Windows::Data::Json::JsonObject;
	using rescue::mascot::json::Number;
	constexpr auto Color = rescue::mascot::json::Vector3;	// 色は [r, g, b] の 3 つ組

	// テクスチャ（{ "index": n }）の n。無ければ nullopt
	std::optional<std::size_t> Texture(const JsonObject& object, std::wstring_view name) {
		if (!object.HasKey(name)) return std::nullopt;
		return static_cast<std::size_t>(object.GetNamedObject(name).GetNamedNumber(L"index"));
	}
}


namespace rescue::mascot {

	MToonMaterial ParseMToon(std::string_view json) {
		// 1. 文字列（UTF-8）を JSON として読む。読めなければ winrt::hresult_error
		const JsonObject object = JsonObject::Parse(winrt::to_hstring(json));

		// 2. 項目ごとに読む。既定値は MToonMaterial に書いてある値（m の今の値）をそのまま渡す
		MToonMaterial m;
		m.shadeColor = Color(object, L"shadeColorFactor", m.shadeColor);
		m.shadeMultiplyTexture = Texture(object, L"shadeMultiplyTexture");
		m.shadingShift = Number(object, L"shadingShiftFactor", m.shadingShift);
		m.shadingShiftTexture = Texture(object, L"shadingShiftTexture");
		if (m.shadingShiftTexture) {
			m.shadingShiftTextureScale = Number(object.GetNamedObject(L"shadingShiftTexture"), L"scale", m.shadingShiftTextureScale);
		}
		m.shadingToony = Number(object, L"shadingToonyFactor", m.shadingToony);
		m.giEqualization = Number(object, L"giEqualizationFactor", m.giEqualization);

		m.matcapColor = Color(object, L"matcapFactor", m.matcapColor);
		m.matcapTexture = Texture(object, L"matcapTexture");
		m.rimColor = Color(object, L"parametricRimColorFactor", m.rimColor);
		m.rimFresnelPower = Number(object, L"parametricRimFresnelPowerFactor", m.rimFresnelPower);
		m.rimLift = Number(object, L"parametricRimLiftFactor", m.rimLift);
		m.rimMultiplyTexture = Texture(object, L"rimMultiplyTexture");
		m.rimLightingMix = Number(object, L"rimLightingMixFactor", m.rimLightingMix);

		// 3. 輪郭線の決め方（文字列 → OutlineWidthMode）。知らない文字列は輪郭線なし
		const winrt::hstring mode = object.GetNamedString(L"outlineWidthMode", L"none");
		m.outlineWidthMode = mode == L"worldCoordinates" ? OutlineWidthMode::WorldCoordinates
			: mode == L"screenCoordinates" ? OutlineWidthMode::ScreenCoordinates
			: OutlineWidthMode::None;

		m.outlineWidth = Number(object, L"outlineWidthFactor", m.outlineWidth);
		m.outlineWidthMultiplyTexture = Texture(object, L"outlineWidthMultiplyTexture");
		m.outlineColor = Color(object, L"outlineColorFactor", m.outlineColor);
		m.outlineLightingMix = Number(object, L"outlineLightingMixFactor", m.outlineLightingMix);

		m.renderQueueOffset = static_cast<int>(Number(object, L"renderQueueOffsetNumber", 0.0f));
		return m;
	}
}
