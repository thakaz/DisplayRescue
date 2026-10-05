// DisplayRescue — 入口とサブコマンドの振り分け (C++20)
//
// 使い方:
//   DisplayRescueService.exe                      → 使い方を表示（help / -h / /? も同じ）
//   DisplayRescueService.exe version              → バージョンを表示
//   DisplayRescueService.exe extend | clone | internal | external
//   DisplayRescueService.exe dump [all]
//   DisplayRescueService.exe bindings             → キーの割り当ての一覧（一般ユーザーで実行してよい）
//   DisplayRescueService.exe bind <action> <keys> [--force]
//                                          → 管理者で実行。割り当てを書き込む（例: bind extend Ctrl+Alt+F9）。
//                                            普段の入力を奪うキー（Ctrl も Alt も無い A など）は --force が要る
//   DisplayRescueService.exe unbind <action>      → 管理者で実行。割り当てを外す
//   DisplayRescueService.exe watch [ハンドル]     → 割り当てたキーを待ち、押されたらその場で切り替える（手で動かすときはハンドルなし、Ctrl+C で終了。
//                                            サービスは継承させた「止めて」のイベントの値を付けて起動する）
//   DisplayRescueService.exe service              → SCM から起動される（手では実行しない）
//   DisplayRescueService.exe install              → 管理者で実行。Program Filesにコピーしてサービスを登録・起動する。
//   DisplayRescueService.exe uninstall            → 管理者で実行。サービスとキーの設定を削除し、Program Files の exe を消す
//
// 終了コード: 0 = 成功、それ以外 = Win32 エラーコード（引数の誤りは ERROR_BAD_ARGUMENTS (160)）

#include <windows.h>
#include <fcntl.h>   // _O_U8TEXT
#include <io.h>      // _setmode
#include <wil/result.h>  // wil::ResultFromCaughtException

#include <algorithm>
#include <cstdio>
#include <cwchar>           // std::wcstoul
#include <format>
#include <iostream>
#include <string_view>

#include "common/Common.h"
#include "common/Log.h"
#include "config/Settings.h"
#include "ccd/DisplayConfig.h"
#include "ccd/Dump.h"
#include "ccd/Topology.h"
#include "input/Watcher.h"
#include "service/Installer.h"
#include "service/Service.h"
#include "Version.h"

namespace {
	using namespace rescue;

	// 利用者向けの使い方。service / watch はサービスが使う内部用なので載せない
	constexpr std::wstring_view kUsage =
		L"DisplayRescue " DR_VERSION_STRING L" — キー一発で画面構成を戻します(ログイン画面・ロック画面でも)\n"
		L"\n"
		L"インストールとキーの設定は、同じフォルダのDisplayRescue.exe(設定画面)からできます。\n"
		L"コマンドで行う場合:\n"
		L"  DisplayRescueService install              管理者で実行。サービスとして登録し、キーで切り替えられるようにする\n"
		L"  DisplayRescueService uninstall            管理者で実行。サービスとキーの設定を削除する\n"
		L"  DisplayRescueService bindings             キーの割り当てを表示する\n"
		L"  DisplayRescueService bind <操作> <キー>   管理者で実行。割り当てを変える(例: bind clone Ctrl+Alt+F10、bind extend F13)\n"
		L"  DisplayRescueService unbind <操作>        管理者で実行。割り当てを外す\n"
		L"  DisplayRescueService <操作>               今すぐ切り替える\n"
		L"  DisplayRescueService dump [all]           今の画面構成を表示する\n"
		L"  DisplayRescueService version              バージョンを表示する\n"
		L"\n"
		L"操作: extend(拡張) / clone(複製) / internal(メインのみ) / external(サブのみ)\n"
		L"既定のキー: Ctrl+Alt+F9で拡張、Ctrl+Alt+F10で複製、Ctrl+Alt+F11でメインのみ、Ctrl+Alt+F12でサブのみ\n";

	//catch(...)の中で呼ぶ。例外をHRESULTにしてメッセージを出し、終了コードを返す。
	int ReportCaughtError(std::wstring_view what) {
		const HRESULT hr = wil::ResultFromCaughtException();
		std::wcout << std::format(L"{}に失敗しました: {}\n", what, DescribeHresult(hr));
		if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) {
			std::wcout << L"管理者として実行したPowerShellから実行してください。\n";
		}
		return ExitCodeFromHresult(hr);
	}

	void PrintLine(std::wstring_view line) {
		std::wcout << line << L'\n';
	}

	// install / uninstall。管理者で実行する。途中経過はコンソールに出し、失敗の詳細（どの API か）は WIL が rescue.log に書く
	int RunServiceSetup(std::wstring_view what, void (*setup)(const service::Progress&)) {
		RouteWilFailuresToLog();
		try {
			setup(PrintLine);
			return 0;
		}
		catch (...) {
			return ReportCaughtError(what);
		}
	}

	// action 名を Topology にする。不明ならメッセージを出して nullopt
	std::optional<ccd::Topology> ParseActionOrReport(std::wstring_view name) {
		const auto topology = ccd::ParseTopology(name);
		if (!topology) {
			std::wcout << std::format(L"不明な操作です: {}(extend / clone / internal / external)\n", name);
		}
		return topology;
	}

	//bind / unbind の後に呼ぶ。待ち受け役は起動時にしか割り当てを読まないので、サービスに置き直してもらう
	void ApplyToService() {
		if (service::RequestServiceReload()) {
			std::wcout << L"サービスに反映しました。\n";
		}
		else {
			std::wcout << L"サービスが動いていないため、次にサービスが起動したときから有効になります。\n";
		}
	}

	// 今の割り当ての一覧。一般ユーザーで実行してよい（HKLM は読むだけなら誰でもできる）
	int RunBindings() {
		try {
			const auto bindings = config::LoadBindings();
			for (const auto topology : ccd::AllTopologies()) {
				const auto it = std::ranges::find(bindings, topology, &config::Binding::topology);
				const std::wstring keys = it != bindings.end() ? config::FormatHotkey(it->hotkey) : L"(割り当てなし)";
				std::wcout << std::format(L"{:<9} {}\n", ccd::NameOf(topology), keys);
			}
			return 0;
		}
		catch (...) {
			return ReportCaughtError(L"設定の読み込み");
		}
	}

	//bind <action> <keys> [--force]。管理者で実行する。
	int RunBind(std::wstring_view actionName, std::wstring_view keysText, bool force) {
		const auto topology = ParseActionOrReport(actionName);
		if (!topology) return ERROR_BAD_ARGUMENTS;

		const auto hotkey = config::ParseHotkey(keysText);
		if (!hotkey) {
			std::wcout << std::format(L"キーの書き方が正しくありません: {}(例: Ctrl+Alt+F9、F13、Ctrl+Shift+Pause、Alt+0xBA)\n", keysText);
			return ERROR_BAD_ARGUMENTS;
		}
		// 止めはしないが、知らずに自分を締め出さないよう、確認（--force）を求める
		if (config::StealsNormalInput(*hotkey) && !force) {
			const auto keys = config::FormatHotkey(*hotkey);
			std::wcout << std::format(
				L"{}にはCtrlもAltも付いていないため、割り当てるとこのキーがWindows全体で使えなくなります。\n"
				L"ログイン画面のパスワード入力でも打てなくなり、サインインできなくなるおそれがあります。\n"
				L"それでもよければ --forceを付けて実行してください: DisplayRescueService bind {} {} --force\n",
				keys, ccd::NameOf(*topology), keys);
			return ERROR_CANCELLED;
		}
		try {
			if (const auto conflict = config::SaveBinding({ *topology, *hotkey })) {
				const auto other = ccd::NameOf(*conflict);
				std::wcout << std::format(L"{}はすでに{}に割り当てられています。先にunbind {}してください。\n", config::FormatHotkey(*hotkey), other, other);
				return ERROR_ALREADY_EXISTS;
			}
			std::wcout << std::format(L"{} → {}\n", ccd::NameOf(*topology), config::FormatHotkey(*hotkey));
			ApplyToService();
			return 0;
		}
		catch (...) {
			return ReportCaughtError(L"設定の書き込み");
		}
	}

	//unbind <action>。管理者で実行する。
	int RunUnbind(std::wstring_view actionName) {
		const auto topology = ParseActionOrReport(actionName);
		if (!topology) return ERROR_BAD_ARGUMENTS;

		try {
			if (config::RemoveBinding(*topology)) {
				std::wcout << std::format(L"{}の割り当てを外しました。\n", ccd::NameOf(*topology));
				ApplyToService();
			}
			else {
				std::wcout << std::format(L"{}には元から割り当てがありません。\n", ccd::NameOf(*topology));
			}
			return 0;
		}
		catch (...) {
			return ReportCaughtError(L"設定の書き込み");
		}
	}

	// std::wcout に日本語を出せるようにする。
	// 既定のままだと wcout は日本語を変換できず、そこで壊れて以降何も出力しなくなる。
	// stdout を UTF-8 のテキストモードにすると、コンソールには正しく表示され、リダイレクト先には UTF-8 で書かれる。
	// SCM から起動されたとき（service）は stdout がつながっていない（_fileno が負）ので何もしない。
	void SetupConsoleOutput()
	{
		if (_fileno(stdout) >= 0)
		{
			(void)_setmode(_fileno(stdout), _O_U8TEXT);
		}
	}

	// <操作>: 今すぐ切り替える
	int RunTopology(ccd::Topology topology)
	{
		const LONG rc = ccd::ApplyTopology(topology);
		std::wcout << std::format(L"{}: {} ({})\n", ccd::NameOf(topology), DescribeError(rc), rc);
		return static_cast<int>(rc);
	}

	int RunDump(bool all)
	{
		try
		{
			ccd::PrintDisplayConfig(ccd::Query(all ? QDC_ALL_PATHS : QDC_ONLY_ACTIVE_PATHS));
			return 0;
		}
		catch (...)
		{
			return ReportCaughtError(L"構成の取得");
		}
	}

	// watch [ハンドル]。ハンドルはサービスが継承させた「止めて」のイベントの値（10 進）
	int RunWatchCommand(int argc, wchar_t* argv[])
	{
		HANDLE stopEvent = nullptr;
		if (argc >= 3) {
			wchar_t* end = nullptr;
			const unsigned long value = std::wcstoul(argv[2], &end, 10); //10進で読む。endは読めなかった最初の文字を指す
			if (end == argv[2] || *end != L'\0' || value == 0) return ERROR_BAD_ARGUMENTS;
			stopEvent = ULongToHandle(value);	//継承したハンドルは親と同じ値のまま使える。
		}
		return input::RunWatch(stopEvent);
	}
}

int wmain(int argc, wchar_t* argv[])
{
	SetupConsoleOutput();

	// 引数なしは使い方だけ出す（配布した exe をダブルクリックしただけで画面が切り替わらないように）
	if (argc < 2) {
		std::wcout << kUsage;
		return 0;
	}
	const std::wstring_view command = argv[1];
	const auto is = [command](std::wstring_view name) { return EqualsIgnoreCase(command, name); };

	if (is(L"help") || command == L"-h" || command == L"/?" || command == L"--help") {
		std::wcout << kUsage;
		return 0;
	}
	if (is(L"version")) {
		const auto installed = service::InstalledVersion();
		std::wcout << std::format(L"DisplayRescue {}(インストール済み: {})\n",
			CurrentVersion().ToString(), installed ? installed->ToString() : L"なし");
		return 0;
	}

	if (is(L"install")) return RunServiceSetup(L"インストール", service::Install);
	if (is(L"uninstall")) return RunServiceSetup(L"アンインストール", service::Uninstall);
	if (is(L"service")) return service::RunService();
	if (is(L"watch")) return RunWatchCommand(argc, argv);

	if (is(L"bindings")) return RunBindings();
	if (is(L"bind")) {
		const bool force = argc == 5 && EqualsIgnoreCase(argv[4], L"--force");
		if (argc != 4 && !force) {
			std::wcout << L"使い方: DisplayRescueService bind <操作> <キー> [--force](例: bind extend Ctrl+Alt+F9)\n";
			return ERROR_BAD_ARGUMENTS;
		}
		return RunBind(argv[2], argv[3], force);
	}
	if (is(L"unbind")) {
		if (argc != 3) {
			std::wcout << L"使い方: DisplayRescueService unbind <操作>\n";
			return ERROR_BAD_ARGUMENTS;
		}
		return RunUnbind(argv[2]);
	}

	if (is(L"dump")) {
		const bool all = argc >= 3 && EqualsIgnoreCase(argv[2], L"all");
		return RunDump(all);
	}

	if (const auto topology = ccd::ParseTopology(command)) return RunTopology(*topology);

	std::wcout << std::format(L"不明なコマンドです: {}\n\n", command) << kUsage;
	return ERROR_BAD_ARGUMENTS;
}
