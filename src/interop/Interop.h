#pragma once

// DisplayRescueInterop.dll の入口。設定アプリ（C#）が P/Invoke で呼ぶ（C# 側は src/settings/Interop/NativeMethods.cs）。
// 決まりごと（キーの名前と決まり、割り当ての読み書き、サービスの状態）は Core（C++）の 1 か所に置き、C# には書き写さない。
// PowerToys の PowerToys.Interop にあたる。
//
// 境界の約束（関数を足すときも守る）:
// - extern "C": C++ の名前の飾り（マングリング）を付けず、関数の名前そのままで公開する。C# は名前で探す
// - 例外を外に出さない（noexcept）。C# は C++ の例外を受け取れず、プロセスごと落ちる。失敗は HRESULT（std::int32_t）で返す
// - 型は C# と大きさがそろうものだけ（std::int32_t、std::uint32_t、wchar_t* = UTF-16、下の DR_Binding のような数だけの構造体）。
//   std::wstring や std::vector は渡せない。配列は「先頭のアドレス + 個数」
// - 文字列を返すときは、呼ぶ側のバッファに書く「2 段階」: 戻り値は終端の 0 を含めた必要な文字数。capacity が足りなければ何も書かない
//   （呼ぶ側は大きさを聞いてから取り直す）
// - 呼び出し規約は書かない。x64 の Windows には 1 種類しかない（32 ビットの __stdcall / __cdecl の区別が無い）

#include <cstdint>

// DLL を作るときは公開（dllexport）、使う側の C++ からは取り込み（dllimport）。DR_INTEROP_EXPORTS は DisplayRescueInterop.vcxproj が定義する
#ifdef DR_INTEROP_EXPORTS
#define DR_API extern "C" __declspec(dllexport)
#else
#define DR_API extern "C" __declspec(dllimport)
#endif

// 割り当て 1 件。topology は ccd::Topology の番号（0 拡張 / 1 複製 / 2 メインのみ / 3 サブのみ）、modifiers は MOD_CONTROL などの組み合わせ、vk は仮想キー。
// 並びと大きさは C# の NativeMethods.Binding と同じ（int・uint・uint、12 バイト）
struct DR_Binding {
	std::int32_t topology;
	std::uint32_t modifiers;
	std::uint32_t vk;
};

// ---- 版 ----

// この DLL の版（src/Version.h）。設定アプリが、自分と同じ版の DLL を読んだかを確かめる
DR_API void DisplayRescue_GetVersion(std::int32_t* major, std::int32_t* minor, std::int32_t* patch) noexcept;

// インストール済みの版（Program Files の DisplayRescueService.exe のバージョン情報）。インストールされていなければ 0、あれば 1
DR_API std::int32_t DisplayRescue_GetInstalledVersion(std::int32_t* major, std::int32_t* minor, std::int32_t* patch) noexcept;

// ---- サービス ----

// インストール先のフォルダ（C:\Program Files\DisplayRescue）のパス。2 段階
DR_API std::int32_t DisplayRescue_GetInstallDirectory(wchar_t* buffer, std::int32_t capacity) noexcept;

// サービスの状態。service::ServiceState の番号（0 未登録 / 1 停止 / 2 実行中 / 3 切り替わりの途中 / 4 不明）
DR_API std::int32_t DisplayRescue_QueryServiceState() noexcept;

// サービスに「設定を読み直して」と頼む（130）。送れたら 1、サービスが無い・止まっていれば 0
DR_API std::int32_t DisplayRescue_RequestServiceReload() noexcept;

// ---- キー ----

// config::IsValidHotkey / StealsNormalInput / IsModifierKey。当てはまれば 1
DR_API std::int32_t DisplayRescue_IsValidHotkey(std::uint32_t modifiers, std::uint32_t vk) noexcept;
DR_API std::int32_t DisplayRescue_StealsNormalInput(std::uint32_t modifiers, std::uint32_t vk) noexcept;
DR_API std::int32_t DisplayRescue_IsModifierKey(std::uint32_t vk) noexcept;

// "Ctrl+Alt+F9" のような名前（config::FormatHotkey）を buffer に書く。戻り値は終端を含めた必要な文字数（2 段階）
DR_API std::int32_t DisplayRescue_FormatHotkey(std::uint32_t modifiers, std::uint32_t vk, wchar_t* buffer, std::int32_t capacity) noexcept;

// ---- 割り当て ----

// 今の割り当て（config::LoadBindings。設定が無ければ既定）を bindings に書き、件数を count に入れる。戻り値は HRESULT。
// capacity が足りなければ書かずに HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)（count には必要な件数）
DR_API std::int32_t DisplayRescue_LoadBindings(DR_Binding* bindings, std::int32_t capacity, std::int32_t* count) noexcept;

// 既定の割り当て（config::DefaultBindings）。書き方は LoadBindings と同じ
DR_API std::int32_t DisplayRescue_DefaultBindings(DR_Binding* bindings, std::int32_t capacity, std::int32_t* count) noexcept;

// 割り当てをまとめて置き換える（config::ReplaceBindings。無い操作は割り当てなし）。戻り値は HRESULT（管理者でなければアクセス拒否）。
// 同じキーが 2 つの操作にあれば何も書かず、conflict にその操作の番号を入れる（無ければ -1）
DR_API std::int32_t DisplayRescue_ReplaceBindings(const DR_Binding* bindings, std::int32_t count, std::int32_t* conflict) noexcept;

// ---- マスコット ----

// マスコットの設定（config::MascotSettings）。並びと大きさは C# の NativeMethods.MascotSettings と同じ。数だけの構造体
struct DR_MascotSettings {
	std::int32_t enabled;				// 0 / 1
	std::int32_t showOnDesktop;			// 0 / 1
	std::int32_t positionX;				// -1000〜2000（0〜1000 が画面の中）
	std::int32_t positionY;
	std::int32_t scalePercent;
	std::int32_t yawDegrees;
	std::int32_t pitchDegrees;
	std::int32_t motionIntervalSeconds;
	std::int32_t fadeMilliseconds;
	std::int32_t idleFadeMilliseconds;
	std::int32_t showShortcuts;			// 0 / 1（画面切り替えキーの案内。後から追加したため最後に置く）
	std::int32_t keepResident;			// 0 / 1（常駐させる）
};

// 読む（設定が無ければ既定値）。戻り値は HRESULT
DR_API std::int32_t DisplayRescue_LoadMascotSettings(DR_MascotSettings* settings) noexcept;

// 書く（範囲は C++ 側で収める）。管理者でなければアクセス拒否。戻り値は HRESULT
DR_API std::int32_t DisplayRescue_SaveMascotSettings(const DR_MascotSettings* settings) noexcept;

// モデルのファイル（%ProgramData%\DisplayRescue\mascot.model。取り込むときに変換したもの）とモーションのフォルダ（*.motion）のパス。2 段階
DR_API std::int32_t DisplayRescue_GetMascotModelPath(wchar_t* buffer, std::int32_t capacity) noexcept;
DR_API std::int32_t DisplayRescue_GetMascotMotionDirectory(wchar_t* buffer, std::int32_t capacity) noexcept;
// 待機モーションのファイル（%ProgramData%\DisplayRescue\mascot-idle.motion）のパス。2 段階
DR_API std::int32_t DisplayRescue_GetMascotIdleMotionPath(wchar_t* buffer, std::int32_t capacity) noexcept;
// 取り込んだ元のファイルの置き場所（%ProgramData%\DisplayRescue\mascot-sources）。2 段階
DR_API std::int32_t DisplayRescue_GetMascotSourceDirectory(wchar_t* buffer, std::int32_t capacity) noexcept;
// 変換した形式の版（config::kMascotAssetVersion）。ファイルの 9〜12 バイト目と比べ、違えば変換し直す
DR_API std::int32_t DisplayRescue_GetMascotAssetVersion() noexcept;

// モデルやモーションをコピーする前に呼ぶ。置き場所（%ProgramData%\DisplayRescue）を管理者と SYSTEM だけが書ける状態にし
// （SecureLogDirectory。install と同じ）、モーションのフォルダと元のファイルのフォルダを作る。一般ユーザーが置いたファイルを SYSTEM のマスコットに読ませないため。
// 戻り値は HRESULT
DR_API std::int32_t DisplayRescue_PrepareMascotDirectory() noexcept;

// ---- エラー ----

// HRESULT を "ERROR_ACCESS_DENIED" のような名前にする（rescue::DescribeHresult）。2 段階
DR_API std::int32_t DisplayRescue_DescribeHresult(std::int32_t hr, wchar_t* buffer, std::int32_t capacity) noexcept;
