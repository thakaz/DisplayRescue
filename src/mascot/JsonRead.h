#pragma once

// VRM の拡張の JSON（C++/WinRT の Windows.Data.Json）から値を読む小さな関数。MToon.cpp と SpringBone.cpp が使う
#include <DirectXMath.h>
#include <winrt/windows.foundation.collections.h>
#include <winrt/windows.data.json.h>

#include <string_view>

namespace rescue::mascot::json {
	// 数。無ければ fallback
	inline float Number(const winrt::Windows::Data::Json::JsonObject& object, std::wstring_view name, float fallback) {
		return static_cast<float>(object.GetNamedNumber(name, fallback));
	}

	// 3 つ組の数（[x, y, z] や [r, g, b]）。無ければ fallback
	inline DirectX::XMFLOAT3 Vector3(const winrt::Windows::Data::Json::JsonObject& object, std::wstring_view name, DirectX::XMFLOAT3 fallback) {
		if (!object.HasKey(name)) return fallback;
		const winrt::Windows::Data::Json::JsonArray values = object.GetNamedArray(name);
		return {
			static_cast<float>(values.GetNumberAt(0)),
			static_cast<float>(values.GetNumberAt(1)),
			static_cast<float>(values.GetNumberAt(2)),
		};
	}
}
