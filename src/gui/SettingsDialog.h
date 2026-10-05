#pragma once

#include <windows.h>

namespace rescue::gui {
	// 設定ダイアログを出し、閉じられるまで返らない（モーダル）
	void ShowSettingsDialog(HINSTANCE instance);
}
