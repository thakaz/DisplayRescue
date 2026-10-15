// マスコットの設定の読み書き。config/Settings.cpp（キーの割り当て）と同じ形（wil::reg）。
// 値は REG_DWORD（数）。読めない値は既定値にして、ほかの値まで道連れにしない
#include "config/MascotSettings.h"

#include <windows.h>
#include <ShlObj.h>			// SHGetKnownFolderPath
#include <wil/registry.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <cstdint>
#include <optional>

namespace {
	namespace config = rescue::config;

	constexpr wchar_t kMascotKey[] = L"SOFTWARE\\DisplayRescue\\Mascot";
	constexpr wchar_t kEnabled[] = L"Enabled";
	constexpr wchar_t kShowOnDesktop[] = L"ShowOnDesktop";
	constexpr wchar_t kShowShortcuts[] = L"ShowShortcuts";
	constexpr wchar_t kKeepResident[] = L"KeepResident";
	constexpr wchar_t kPositionX[] = L"PositionX";
	constexpr wchar_t kPositionY[] = L"PositionY";
	constexpr wchar_t kScalePercent[] = L"ScalePercent";
	constexpr wchar_t kYawDegrees[] = L"YawDegrees";
	constexpr wchar_t kPitchDegrees[] = L"PitchDegrees";
	constexpr wchar_t kMotionInterval[] = L"MotionIntervalSeconds";
	constexpr wchar_t kFade[] = L"FadeMilliseconds";
	constexpr wchar_t kIdleFade[] = L"IdleFadeMilliseconds";

	// SYSTEM のマスコットが読むのは変換した形式だけ。元の .vrm / .vrma は kSourceFolderName に取っておく
	constexpr wchar_t kModelFileName[] = L"mascot.model";
	constexpr wchar_t kMotionFolderName[] = L"mascot-motions";
	constexpr wchar_t kIdleMotionFileName[] = L"mascot-idle.motion";
	constexpr wchar_t kSourceFolderName[] = L"mascot-sources";

	// 1 つ読む。無い・型が違う（try_get は型が違うと例外）なら nullopt
	std::optional<std::uint32_t> ReadDword(HKEY key, const wchar_t* name) noexcept {
		try {
			return wil::reg::try_get_value_dword(key, name);
		}
		catch (...) {
			return std::nullopt;
		}
	}

	// 範囲に収める（読み込みと保存の両方で通す）
	config::MascotSettings Clamp(config::MascotSettings s) {
		s.positionX = std::clamp(s.positionX, config::kMinPosition, config::kMaxPosition);
		s.positionY = std::clamp(s.positionY, config::kMinPosition, config::kMaxPosition);
		s.scalePercent = std::clamp(s.scalePercent, config::kMinScalePercent, config::kMaxScalePercent);
		s.yawDegrees = std::clamp(s.yawDegrees, -config::kMaxYawDegrees, config::kMaxYawDegrees);
		s.pitchDegrees = std::clamp(s.pitchDegrees, -config::kMaxPitchDegrees, config::kMaxPitchDegrees);
		s.motionIntervalSeconds = std::clamp(s.motionIntervalSeconds, config::kMinMotionIntervalSeconds, config::kMaxMotionIntervalSeconds);
		s.fadeMilliseconds = std::clamp(s.fadeMilliseconds, 0, config::kMaxFadeMilliseconds);
		s.idleFadeMilliseconds = std::clamp(s.idleFadeMilliseconds, 0, config::kMaxFadeMilliseconds);
		return s;
	}

	std::filesystem::path DataDirectory() {
		wil::unique_cotaskmem_string programData;
		THROW_IF_FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, programData.put()));
		return std::filesystem::path{ programData.get() } / L"DisplayRescue";
	}
}

namespace rescue::config {

	MascotSettings LoadMascotSettings() {
		MascotSettings settings;
		wil::unique_hkey key;
		const HRESULT hr = wil::reg::open_unique_key_nothrow(HKEY_LOCAL_MACHINE, kMascotKey, key, wil::reg::key_access::read);
		if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return settings;
		THROW_IF_FAILED(hr);

		if (const auto v = ReadDword(key.get(), kEnabled)) settings.enabled = *v != 0;
		if (const auto v = ReadDword(key.get(), kShowOnDesktop)) settings.showOnDesktop = *v != 0;
		if (const auto v = ReadDword(key.get(), kShowShortcuts)) settings.showShortcuts = *v != 0;
		if (const auto v = ReadDword(key.get(), kKeepResident)) settings.keepResident = *v != 0;
		// 負になりうる値（位置・向き）も REG_DWORD に入れる。符号なしの 32 ビットとして書き、読むときに符号付きに戻す（ビットの並びは同じ）
		if (const auto v = ReadDword(key.get(), kPositionX)) settings.positionX = static_cast<std::int32_t>(*v);
		if (const auto v = ReadDword(key.get(), kPositionY)) settings.positionY = static_cast<std::int32_t>(*v);
		if (const auto v = ReadDword(key.get(), kScalePercent)) settings.scalePercent = static_cast<std::int32_t>(*v);
		if (const auto v = ReadDword(key.get(), kYawDegrees)) settings.yawDegrees = static_cast<std::int32_t>(*v);
		if (const auto v = ReadDword(key.get(), kPitchDegrees)) settings.pitchDegrees = static_cast<std::int32_t>(*v);
		if (const auto v = ReadDword(key.get(), kMotionInterval)) settings.motionIntervalSeconds = static_cast<int>(*v);
		if (const auto v = ReadDword(key.get(), kFade)) settings.fadeMilliseconds = static_cast<int>(*v);
		if (const auto v = ReadDword(key.get(), kIdleFade)) settings.idleFadeMilliseconds = static_cast<int>(*v);
		return Clamp(settings);
	}

	void SaveMascotSettings(const MascotSettings& input) {
		const MascotSettings s = Clamp(input);
		// 途中の SOFTWARE\DisplayRescue も無ければ一緒に作る。一般ユーザーは ERROR_ACCESS_DENIED
		const auto key = wil::reg::create_unique_key(HKEY_LOCAL_MACHINE, kMascotKey, wil::reg::key_access::readwrite);
		wil::reg::set_value_dword(key.get(), kEnabled, s.enabled ? 1 : 0);
		wil::reg::set_value_dword(key.get(), kShowOnDesktop, s.showOnDesktop ? 1 : 0);
		wil::reg::set_value_dword(key.get(), kShowShortcuts, s.showShortcuts ? 1 : 0);
		wil::reg::set_value_dword(key.get(), kKeepResident, s.keepResident ? 1 : 0);
		wil::reg::set_value_dword(key.get(), kPositionX, static_cast<std::uint32_t>(s.positionX));
		wil::reg::set_value_dword(key.get(), kPositionY, static_cast<std::uint32_t>(s.positionY));
		wil::reg::set_value_dword(key.get(), kScalePercent, static_cast<std::uint32_t>(s.scalePercent));
		wil::reg::set_value_dword(key.get(), kYawDegrees, static_cast<std::uint32_t>(s.yawDegrees));
		wil::reg::set_value_dword(key.get(), kPitchDegrees, static_cast<std::uint32_t>(s.pitchDegrees));
		wil::reg::set_value_dword(key.get(), kMotionInterval, static_cast<std::uint32_t>(s.motionIntervalSeconds));
		wil::reg::set_value_dword(key.get(), kFade, static_cast<std::uint32_t>(s.fadeMilliseconds));
		wil::reg::set_value_dword(key.get(), kIdleFade, static_cast<std::uint32_t>(s.idleFadeMilliseconds));
	}

	std::filesystem::path MascotModelPath() {
		return DataDirectory() / kModelFileName;
	}

	std::filesystem::path MascotMotionDirectory() {
		return DataDirectory() / kMotionFolderName;
	}

	std::filesystem::path MascotIdleMotionPath() {
		return DataDirectory() / kIdleMotionFileName;
	}

	std::filesystem::path MascotSourceDirectory() {
		return DataDirectory() / kSourceFolderName;
	}
}
