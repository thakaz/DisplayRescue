// DisplayRescueInterop.dll の中身。どれも Core の関数を呼んで、境界の形（数・UTF-16 のバッファ・HRESULT）に直すだけ。
// 例外は境界の内側で止めて HRESULT にし（Guard）、文字列は呼ぶ側のバッファに 2 段階で書く（CopyString）。
// 境界の約束は Interop.h の先頭
#include "interop/Interop.h"

#include <windows.h>
#include <wil/result.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Version.h"
#include "ccd/Topology.h"
#include "common/Common.h"
#include "common/FileVersion.h"
#include "common/Log.h"
#include "config/MascotSettings.h"
#include "config/Hotkey.h"
#include "config/Settings.h"
#include "service/Installer.h"
#include "service/Service.h"

namespace {
	namespace ccd = rescue::ccd;
	namespace config = rescue::config;
	namespace service = rescue::service;

	// 例外を外に出さないための包み。f の中で投げられた例外は HRESULT にして返す（成功は S_OK）
	template <typename F>
	std::int32_t Guard(F&& f) noexcept {
		try {
			f();
			return S_OK;
		}
		catch (...) {
			return wil::ResultFromCaughtException();
		}
	}

	// 文字列を呼ぶ側のバッファに書く（2 段階）。戻り値は終端を含めた必要な文字数。足りなければ何も書かない
	std::int32_t CopyString(std::wstring_view text, wchar_t* buffer, std::int32_t capacity) noexcept {
		const auto needed = static_cast<std::int32_t>(text.size() + 1);
		if (buffer && capacity >= needed) {
			std::ranges::copy(text, buffer);
			buffer[text.size()] = L'\0';
		}
		return needed;
	}

	config::Hotkey ToHotkey(std::uint32_t modifiers, std::uint32_t vk) {
		return { .modifiers = modifiers, .vk = vk };
	}

	// 割り当ての一覧を呼ぶ側の配列に書く（LoadBindings と DefaultBindings で共通）
	std::int32_t CopyBindings(const std::vector<config::Binding>& source, DR_Binding* bindings, std::int32_t capacity, std::int32_t* count) {
		if (count) *count = static_cast<std::int32_t>(source.size());
		if (!bindings || capacity < static_cast<std::int32_t>(source.size())) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
		for (std::size_t i = 0; i < source.size(); ++i) {
			bindings[i] = { static_cast<std::int32_t>(source[i].topology), source[i].hotkey.modifiers, source[i].hotkey.vk };
		}
		return S_OK;
	}
}

void DisplayRescue_GetVersion(std::int32_t* major, std::int32_t* minor, std::int32_t* patch) noexcept {
	// C# の out は、ここでは書き込み先のアドレスになる。nullptr は渡されない約束だが、念のため確かめる
	if (major) *major = DR_VERSION_MAJOR;
	if (minor) *minor = DR_VERSION_MINOR;
	if (patch) *patch = DR_VERSION_PATCH;
}

std::int32_t DisplayRescue_GetInstalledVersion(std::int32_t* major, std::int32_t* minor, std::int32_t* patch) noexcept {
	std::optional<rescue::FileVersion> version;
	if (FAILED(Guard([&] { version = service::InstalledVersion(); })) || !version) return 0;
	if (major) *major = static_cast<std::int32_t>(version->major);
	if (minor) *minor = static_cast<std::int32_t>(version->minor);
	if (patch) *patch = static_cast<std::int32_t>(version->patch);
	return 1;
}

std::int32_t DisplayRescue_GetInstallDirectory(wchar_t* buffer, std::int32_t capacity) noexcept {
	std::wstring text;
	Guard([&] { text = service::InstallDirectory().wstring(); });
	return CopyString(text, buffer, capacity);
}

std::int32_t DisplayRescue_QueryServiceState() noexcept {
	auto state = service::ServiceState::Unknown;
	Guard([&] { state = service::QueryServiceState(); });
	return static_cast<std::int32_t>(state);
}

std::int32_t DisplayRescue_RequestServiceReload() noexcept {
	bool sent = false;
	Guard([&] { sent = service::RequestServiceReload(); });
	return sent ? 1 : 0;
}

std::int32_t DisplayRescue_IsValidHotkey(std::uint32_t modifiers, std::uint32_t vk) noexcept {
	return config::IsValidHotkey(ToHotkey(modifiers, vk)) ? 1 : 0;
}

std::int32_t DisplayRescue_StealsNormalInput(std::uint32_t modifiers, std::uint32_t vk) noexcept {
	return config::StealsNormalInput(ToHotkey(modifiers, vk)) ? 1 : 0;
}

std::int32_t DisplayRescue_IsModifierKey(std::uint32_t vk) noexcept {
	return config::IsModifierKey(vk) ? 1 : 0;
}

std::int32_t DisplayRescue_FormatHotkey(std::uint32_t modifiers, std::uint32_t vk, wchar_t* buffer, std::int32_t capacity) noexcept {
	std::wstring text;
	Guard([&] { text = config::FormatHotkey(ToHotkey(modifiers, vk)); });	// 失敗（メモリ不足くらい）は空の名前
	return CopyString(text, buffer, capacity);
}

std::int32_t DisplayRescue_LoadBindings(DR_Binding* bindings, std::int32_t capacity, std::int32_t* count) noexcept {
	std::int32_t result = S_OK;
	const HRESULT hr = Guard([&] { result = CopyBindings(config::LoadBindings(), bindings, capacity, count); });
	return FAILED(hr) ? hr : result;
}

std::int32_t DisplayRescue_DefaultBindings(DR_Binding* bindings, std::int32_t capacity, std::int32_t* count) noexcept {
	std::int32_t result = S_OK;
	const HRESULT hr = Guard([&] { result = CopyBindings(config::DefaultBindings(), bindings, capacity, count); });
	return FAILED(hr) ? hr : result;
}

std::int32_t DisplayRescue_ReplaceBindings(const DR_Binding* bindings, std::int32_t count, std::int32_t* conflict) noexcept {
	if (conflict) *conflict = -1;
	if (count < 0 || (count > 0 && !bindings)) return E_INVALIDARG;
	return Guard([&] {
		const auto all = ccd::AllTopologies();
		std::vector<config::Binding> converted;
		for (const DR_Binding& b : std::span{ bindings, static_cast<std::size_t>(count) }) {
			THROW_HR_IF(E_INVALIDARG, b.topology < 0 || b.topology >= static_cast<std::int32_t>(all.size()));
			converted.push_back({ all[static_cast<std::size_t>(b.topology)], ToHotkey(b.modifiers, b.vk) });
		}
		if (const auto duplicate = config::ReplaceBindings(converted); duplicate && conflict) {
			*conflict = static_cast<std::int32_t>(*duplicate);
		}
	});
}

std::int32_t DisplayRescue_LoadMascotSettings(DR_MascotSettings* settings) noexcept {
	if (!settings) return E_POINTER;
	return Guard([&] {
		const auto s = config::LoadMascotSettings();
		*settings = {};
		settings->enabled = s.enabled ? 1 : 0;
		settings->showOnDesktop = s.showOnDesktop ? 1 : 0;
		settings->positionX = s.positionX;
		settings->positionY = s.positionY;
		settings->scalePercent = s.scalePercent;
		settings->yawDegrees = s.yawDegrees;
		settings->pitchDegrees = s.pitchDegrees;
		settings->motionIntervalSeconds = s.motionIntervalSeconds;
		settings->fadeMilliseconds = s.fadeMilliseconds;
		settings->idleFadeMilliseconds = s.idleFadeMilliseconds;
		settings->showShortcuts = s.showShortcuts ? 1 : 0;
		settings->keepResident = s.keepResident ? 1 : 0;
	});
}

std::int32_t DisplayRescue_SaveMascotSettings(const DR_MascotSettings* settings) noexcept {
	if (!settings) return E_POINTER;
	return Guard([&] {
		config::MascotSettings s;
		s.enabled = settings->enabled != 0;
		s.showOnDesktop = settings->showOnDesktop != 0;
		s.positionX = settings->positionX;
		s.positionY = settings->positionY;
		s.scalePercent = settings->scalePercent;
		s.yawDegrees = settings->yawDegrees;
		s.pitchDegrees = settings->pitchDegrees;
		s.motionIntervalSeconds = settings->motionIntervalSeconds;
		s.fadeMilliseconds = settings->fadeMilliseconds;
		s.idleFadeMilliseconds = settings->idleFadeMilliseconds;
		s.showShortcuts = settings->showShortcuts != 0;
		s.keepResident = settings->keepResident != 0;
		config::SaveMascotSettings(s);
	});
}

std::int32_t DisplayRescue_GetMascotModelPath(wchar_t* buffer, std::int32_t capacity) noexcept {
	std::wstring text;
	Guard([&] { text = config::MascotModelPath().wstring(); });
	return CopyString(text, buffer, capacity);
}

std::int32_t DisplayRescue_GetMascotMotionDirectory(wchar_t* buffer, std::int32_t capacity) noexcept {
	std::wstring text;
	Guard([&] { text = config::MascotMotionDirectory().wstring(); });
	return CopyString(text, buffer, capacity);
}

std::int32_t DisplayRescue_GetMascotIdleMotionPath(wchar_t* buffer, std::int32_t capacity) noexcept {
	std::wstring text;
	Guard([&] { text = config::MascotIdleMotionPath().wstring(); });
	return CopyString(text, buffer, capacity);
}

std::int32_t DisplayRescue_GetMascotSourceDirectory(wchar_t* buffer, std::int32_t capacity) noexcept {
	std::wstring text;
	Guard([&] { text = config::MascotSourceDirectory().wstring(); });
	return CopyString(text, buffer, capacity);
}

std::int32_t DisplayRescue_GetMascotAssetVersion() noexcept {
	return static_cast<std::int32_t>(config::kMascotAssetVersion);
}

std::int32_t DisplayRescue_PrepareMascotDirectory() noexcept {
	return Guard([&] {
		rescue::SecureLogDirectory();
		// フォルダの ACL は親（SecureLogDirectory が整えた）を引き継ぐ
		std::filesystem::create_directories(config::MascotMotionDirectory());
		std::filesystem::create_directories(config::MascotSourceDirectory() / L"motions");
	});
}

std::int32_t DisplayRescue_DescribeHresult(std::int32_t hr, wchar_t* buffer, std::int32_t capacity) noexcept {
	std::wstring text;
	Guard([&] { text = rescue::DescribeHresult(hr); });
	return CopyString(text, buffer, capacity);
}
