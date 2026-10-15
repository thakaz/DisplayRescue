#include "mascot/Expression.h"

#include <winrt/windows.foundation.collections.h>
#include <winrt/Windows.Data.Json.h>

#include <initializer_list>

#pragma comment(lib,"windowsapp.lib")

namespace {
	using winrt::Windows::Data::Json::IJsonValue;
	using winrt::Windows::Data::Json::JsonObject;

	// 1. 表情 1 つ。どのモーフをどれだけ動かすか（morphTargetBinds）
	rescue::mascot::VrmExpression ReadExpression(std::wstring_view name, const JsonObject& object) {
		rescue::mascot::VrmExpression expression;
		expression.name = name;
		expression.isBinary = object.GetNamedBoolean(L"isBinary", false);
		if (!object.HasKey(L"morphTargetBinds")) return expression;

		// 2. 配列を範囲 for で回す
		for (const IJsonValue& value : object.GetNamedArray(L"morphTargetBinds")) {
			const JsonObject bind = value.GetObject();
			expression.binds.push_back({
				static_cast<std::size_t>(bind.GetNamedNumber(L"node")),
				static_cast<std::size_t>(bind.GetNamedNumber(L"index")),
				static_cast<float>(bind.GetNamedNumber(L"weight",1.0)),
				});
		}
		return expression;
	}
}

namespace rescue::mascot {

	std::vector<VrmExpression> ParseExpressions(std::string_view json) {
		const JsonObject vrm = JsonObject::Parse(winrt::to_hstring(json));
		std::vector<VrmExpression> expressions;
		if (!vrm.HasKey(L"expressions")) return expressions;
		const JsonObject all = vrm.GetNamedObject(L"expressions");

		// 3. preset（blink など決まった名前）と custom（モデルが付けた名前）を分けずに並べる
		for (const std::wstring_view group : {L"preset", L"custom"}) {
			if (!all.HasKey(group)) continue;
			for (const auto& [name, value] : all.GetNamedObject(group)) {
				expressions.push_back(ReadExpression(name, value.GetObject()));
			}
		}
		return expressions;
	}
}