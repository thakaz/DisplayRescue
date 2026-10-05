#include "config/Settings.h"

#include <windows.h>
#include <wil/registry.h>   // wil::reg::open_unique_key_nothrow / create_unique_key / try_get_value_string
#include <wil/result.h>

#include <algorithm>
#include <string>

namespace {
	namespace ccd = rescue::ccd;
	namespace config = rescue::config;

	// 値の名前は action 名（extend / clone / internal / external）、データは "Ctrl+Alt+F9" 形式の REG_SZ
	constexpr wchar_t kSettingsKey[] = L"SOFTWARE\\DisplayRescue";
	constexpr wchar_t kBindingsKey[] = L"SOFTWARE\\DisplayRescue\\Bindings";

	// 値の名前に渡すので NUL 終端の文字列にする（string_view は終端が保証されない）
	std::wstring ValueName(ccd::Topology topology) {
		return std::wstring{ ccd::NameOf(topology) };
	}

	// 1 件読む。無い・読めない（REG_SZ でない、書式が違う）なら nullopt。
	// try_get_value_string は「値が無い」ときだけ nullopt で、型が違うと例外になるので、ここで受け止める
	std::optional<config::Hotkey> ReadHotkey(HKEY key, ccd::Topology topology) noexcept {
		try {
			const auto text = wil::reg::try_get_value_string(key, ValueName(topology).c_str());
			if (!text) return std::nullopt;
			return config::ParseHotkey(*text);
		}
		catch (...) {
			return std::nullopt;
		}
	}
}

namespace rescue::config {

	std::vector<Binding> DefaultBindings() {
		constexpr UINT kCtrlAlt = MOD_CONTROL | MOD_ALT;
		return {
			{ ccd::Topology::Extend,	{ kCtrlAlt, VK_F9 } },
			{ ccd::Topology::Clone,		{ kCtrlAlt, VK_F10 } },
			{ ccd::Topology::Internal,	{ kCtrlAlt, VK_F11 } },
			{ ccd::Topology::External,	{ kCtrlAlt, VK_F12 } },
		};
	}

	std::vector<Binding> LoadBindings() {
		wil::unique_hkey key;
		const HRESULT hr = wil::reg::open_unique_key_nothrow(HKEY_LOCAL_MACHINE, kBindingsKey, key, wil::reg::key_access::read);
		if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return DefaultBindings();
		THROW_IF_FAILED(hr);

		std::vector<Binding> bindings;
		for (const auto topology : ccd::AllTopologies()) {
			if (const auto hotkey = ReadHotkey(key.get(), topology)) {
				bindings.push_back({ topology, *hotkey });
			}
		}
		return bindings;
	}

	std::optional<ccd::Topology> ReplaceBindings(std::span<const Binding> bindings)
	{
		for (auto it = bindings.begin(); it != bindings.end(); ++it) {
			const bool duplicated = std::any_of(bindings.begin(), it, [&](const Binding& b) { return b.hotkey == it->hotkey; });
			if (duplicated) return it->topology;
		}

		// 4 つの action すべてについて「書く」か「消す」かを決めるので、キーが無かったとき（＝既定の割り当て）の扱いは要らない。
		// 途中の SOFTWARE\DisplayRescue も無ければ一緒に作る。一般ユーザーは ERROR_ACCESS_DENIED
		const auto key = wil::reg::create_unique_key(HKEY_LOCAL_MACHINE, kBindingsKey, wil::reg::key_access::readwrite);
		for (const auto topology : ccd::AllTopologies()) {
			if (const auto it = std::ranges::find(bindings, topology, &Binding::topology); it != bindings.end()) {
				wil::reg::set_value_string(key.get(), ValueName(topology).c_str(), FormatHotkey(it->hotkey).c_str());
				continue;
			}
			//wil には値を消す関数がないので Win32 を直接呼ぶ。戻り値は Win32 エラーコード
			const LSTATUS rc = RegDeleteValueW(key.get(), ValueName(topology).c_str());
			if (rc != ERROR_FILE_NOT_FOUND) THROW_IF_WIN32_ERROR(rc);
		}
		return std::nullopt;
	}

	std::optional<ccd::Topology> SaveBinding(const Binding& binding)
	{
		auto bindings = LoadBindings();
		//重なりは今の割り当てと比べる。同じ action 同士は置き換えなので重なりではない（今と同じキーで bind し直しても拒否しない）
		const auto conflict = std::ranges::find_if(bindings, [&](const Binding& b) {
			return b.hotkey == binding.hotkey && b.topology != binding.topology;
			});
		if (conflict != bindings.end()) return conflict->topology;

		std::erase_if(bindings, [&](const Binding& b) { return b.topology == binding.topology; });
		bindings.push_back(binding);
		return ReplaceBindings(bindings);
	}

	bool RemoveBinding(ccd::Topology topology)
	{
		auto bindings = LoadBindings();
		if (std::erase_if(bindings, [&](const Binding& b) { return b.topology == topology; }) == 0) return false;
		ReplaceBindings(bindings);
		return true;
	}

	void DeleteAllSettings()
	{
		//RegDeleteTreeW はキーの中身（サブキーと値）ごと消す。戻り値は Win32 エラーコード
		const LSTATUS rc = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kSettingsKey);
		if (rc == ERROR_FILE_NOT_FOUND) return;
		THROW_IF_WIN32_ERROR(rc);
	}
}
