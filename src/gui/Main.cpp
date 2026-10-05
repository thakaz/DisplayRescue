// DisplayRescue.exe — 設定 GUI の入口。
// キーの割り当ての変更とサービスの状態の表示だけを受け持つ。復旧そのもの（サービスと待ち受け役）は GUI に依存しない。
// マニフェストで管理者を要求する（設定は HKLM に書くため）。起動すると UAC の確認が出る

#include <windows.h>
#include <commctrl.h>

#include "common/Log.h"
#include "gui/SettingsDialog.h"

#pragma comment(lib, "comctl32.lib")

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
	// インストールなどの失敗で、どの API が失敗したかを rescue.log に残す
	rescue::RouteWilFailuresToLog();

	// ホットキー入力欄（msctls_hotkey32）はコモンコントロールなので、使う前に登録してもらう
	const INITCOMMONCONTROLSEX controls{ .dwSize = sizeof(controls), .dwICC = ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES };
	InitCommonControlsEx(&controls);

	rescue::gui::ShowSettingsDialog(instance);
	return 0;
}
