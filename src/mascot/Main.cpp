// DisplayRescueMascot.exe の入口。使い方は 4 つ:
//   DisplayRescueMascot.exe <ハンドルの値>
//     サービスが起動する。ハンドルは「止めて」のイベント（ホットキー監視プロセスの "DisplayRescueService.exe watch <ハンドルの値>" と同じ渡し方）。
//     大きさと位置は設定（HKLM\SOFTWARE\DisplayRescue\Mascot）を起動時に読む
//   DisplayRescueMascot.exe --convert-model <.vrm / .glb> <出力> / --convert-motion <.vrma> <出力>
//     設定アプリが取り込むときに起動する。解析して、マスコットが読む形式（AssetFile.h）で書く
//   DisplayRescueMascot.exe --edit <横> <縦> <大きさ> <左右の向き> <前後の傾き>
//     設定アプリの「配置を調整」。画面いっぱいの調整用の窓を出し、ドラッグ・ホイールで調整して Enter で設定に保存する（Esc でやめる）
//   DisplayRescueMascot.exe --preview <.motion> <横> <縦> <大きさ> <左右の向き> <前後の傾き> <切り替え時間（ミリ秒）> <待機に戻る切り替え時間（ミリ秒）>
//     設定アプリの「再生」。そのモーションをデスクトップ上で 1 回再生して終了する。各数値には、保存前でも設定画面の現在値を使用できる
// 引数なしは手で起動したとき。止めてが来ないので、タスクマネージャーなどで終わらせる。
// サブシステムは Windows（コンソールの窓を出さない）なので、入口は wWinMain。引数は CommandLineToArgvW で分ける
#include <windows.h>
#include <shellapi.h>		// CommandLineToArgvW
#include <wil/resource.h>	// wil::unique_hlocal_ptr / unique_handle
#include <wil/result.h>		// wil::ResultFromCaughtException

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

#include <DirectXMath.h>	// XMConvertToRadians

#include "common/Common.h"
#include "config/MascotSettings.h"
#include "mascot/AssetFile.h"
#include "mascot/GltfMesh.h"
#include "mascot/Mascot.h"

namespace {
	namespace config = rescue::config;

	// "-30" → 数（負も読む。位置と向きは負になりうる）。数でなければ nullopt
	std::optional<int> ParseSigned(const wchar_t* text) {
		wchar_t* end = nullptr;
		const long value = std::wcstol(text, &end, 10);
		if (end == text || *end != L'\0') return std::nullopt;
		return static_cast<int>(value);
	}

	rescue::mascot::MascotOptions ToOptions(const config::MascotSettings& settings) {
		rescue::mascot::MascotOptions options;
		options.positionX = settings.positionX / 1000.0f;
		options.positionY = settings.positionY / 1000.0f;
		options.scale = settings.scalePercent / 100.0f;
		options.yaw = DirectX::XMConvertToRadians(static_cast<float>(settings.yawDegrees));
		options.pitch = DirectX::XMConvertToRadians(static_cast<float>(settings.pitchDegrees));
		options.showOnDesktop = settings.showOnDesktop;
		options.showMascot = settings.enabled;
		options.showShortcuts = settings.showShortcuts;
		options.motionIntervalSeconds = settings.motionIntervalSeconds;
		options.fadeSeconds = settings.fadeMilliseconds / 1000.0f;
		options.idleFadeSeconds = settings.idleFadeMilliseconds / 1000.0f;
		return options;
	}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
	// 引数の配列は LocalFree で返す決まりなので、箱に入れる
	int argc = 0;
	const wil::unique_hlocal_ptr<LPWSTR> argv{ CommandLineToArgvW(GetCommandLineW(), &argc) };
	if (!argv) return ERROR_BAD_ARGUMENTS;
	const std::span args{ argv.get(), static_cast<std::size_t>(argc) };

	// 設定アプリが、モデルやモーションを取り込むときに起動する。
	// 他人の作ったファイル（glTF・JSON・画像）の解析はここだけで行い、マスコットが読む形式（AssetFile.h）にして書く。
	// 戻り値は終了コード（0 = 書けた。失敗は HRESULT から作ったエラーコード。設定アプリが名前にして出す）
	if (args.size() == 4 && (std::wstring_view{ args[1] } == L"--convert-model" || std::wstring_view{ args[1] } == L"--convert-motion")) {
		try {
			// 書いたら読み戻して確かめる（マスコットが読めないものを置かない）
			if (std::wstring_view{ args[1] } == L"--convert-model") {
				rescue::mascot::SaveModelAsset(rescue::mascot::LoadGltfMesh(args[2]), args[3]);
				rescue::mascot::LoadModelAsset(args[3]);
			}
			else {
				rescue::mascot::SaveMotionAsset(rescue::mascot::LoadGltfMotion(args[2]), args[3]);
				rescue::mascot::LoadMotionAsset(args[3]);
			}
			return ERROR_SUCCESS;
		}
		catch (...) {
			return rescue::ExitCodeFromHresult(wil::ResultFromCaughtException());
		}
	}

	// 設定アプリの「配置を調整」。調整用の窓を出し、Enter で保存して 0、Esc なら ERROR_CANCELLED。
	// 続く 5 つの数（位置の横・縦、大きさ、左右の向き、前後の傾き。設定と同じ物差し）から始める（設定アプリの画面の今の値）
	if (args.size() == 7 && std::wstring_view{ args[1] } == L"--edit") {
		config::MascotSettings settings;
		try {
			settings = config::LoadMascotSettings();
		}
		catch (...) {
		}
		const auto x = ParseSigned(args[2]);
		const auto y = ParseSigned(args[3]);
		const auto scale = ParseSigned(args[4]);
		const auto yaw = ParseSigned(args[5]);
		const auto pitch = ParseSigned(args[6]);
		if (!x || !y || !scale || !yaw || !pitch) return ERROR_BAD_ARGUMENTS;
		settings.positionX = *x;
		settings.positionY = *y;
		settings.scalePercent = *scale;
		settings.yawDegrees = *yaw;
		settings.pitchDegrees = *pitch;
		auto options = ToOptions(settings);
		options.edit = true;
		return rescue::mascot::RunMascot(nullptr, options);
	}

	// 設定アプリの「再生」。窓はクリック素通しなので止める操作は受けない（設定アプリが止めるときはプロセスを終わらせる）。
	// 読むのは変換した形式（.motion）だけ。他人のファイルの解析はしない
	if (args.size() == 10 && std::wstring_view{ args[1] } == L"--preview") {
		const std::filesystem::path motion{ args[2] };
		if (!rescue::EqualsIgnoreCase(motion.extension().wstring(), L".motion")) return ERROR_BAD_ARGUMENTS;
		std::array<std::optional<int>, 7> numbers;
		for (std::size_t i = 0; i < numbers.size(); ++i) numbers[i] = ParseSigned(args[3 + i]);
		if (std::ranges::any_of(numbers, [](const std::optional<int>& n) { return !n; })) return ERROR_BAD_ARGUMENTS;
		config::MascotSettings settings;
		settings.positionX = *numbers[0];
		settings.positionY = *numbers[1];
		settings.scalePercent = *numbers[2];
		settings.yawDegrees = *numbers[3];
		settings.pitchDegrees = *numbers[4];
		settings.fadeMilliseconds = *numbers[5];
		settings.idleFadeMilliseconds = *numbers[6];
		auto options = ToOptions(settings);
		options.previewMotion = motion;
		return rescue::mascot::RunMascot(nullptr, options);
	}

	HANDLE stopEvent = nullptr;
	if (args.size() >= 2) {
		const auto handle = rescue::ParseInheritedHandle(args[1]);
		if (!handle) return ERROR_BAD_ARGUMENTS;
		stopEvent = *handle;
	}
	// 設定が読めなくても（壊れた値は既定値になるので、まず無いが）既定の大きさと位置で出す
	config::MascotSettings settings;
	try {
		settings = config::LoadMascotSettings();
	}
	catch (...) {
	}
	return rescue::mascot::RunMascot(stopEvent, ToOptions(settings));
}
