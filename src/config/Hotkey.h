#pragma once

#include <windows.h>
#include <optional>
#include <string>
#include <string_view>

namespace rescue::config {
	//キーの組み合わせ一つ。RegisterHotKeyにそのまま渡せる形で持つ。
	struct Hotkey {
		UINT modifiers = 0;    //MOD_CONTROL / MOD_ALT / MOD_SHIFT の組み合わせ（MOD_NOREPEAT は登録するときに足す）
		UINT vk = 0;          //仮想キーコード

		bool operator ==(const Hotkey&) const = default;
	};

	// "Ctrl+Alt+F9" → Hotkey。大文字小文字は問わない。
	// 決まり: 修飾キー（Ctrl / Alt / Shift）とキー名を "+" でつなぐ。キー名はちょうど一つ、最後に置く。
	// キー名: F1〜F24、A〜Z、0〜9、Space・Enter・Left・Pause・Num0 などの名前（Hotkey.cpp の kNamedKeys）、
	//         それ以外は仮想キーコードの 16 進（"0xBA" など。記号のキーは配列で意味が変わるので名前を付けない）。
	// IsValidHotkey に合わなければ nullopt。普段の入力を奪う組み合わせ（StealsNormalInput）も受け付ける（止めるかどうかは呼ぶ側が決める）
	std::optional<Hotkey> ParseHotkey(std::wstring_view text);

	// 修飾キーそのもの（Shift / Ctrl / Alt / Win。左右の別も含む）の仮想キーか
	bool IsModifierKey(UINT vk);

	// ホットキーとして成り立つか。修飾キーは Ctrl / Alt / Shift だけで、キーが 1 つあり、それがマウスのボタンや修飾キーそのものではないこと
	bool IsValidHotkey(const Hotkey& hotkey);

	// 普段の入力や操作で使うキーを奪う組み合わせか（Ctrl も Alt も付いておらず、キーが F13〜F24 ではない）。
	// 登録するとそのキーが Windows 全体で使えなくなり、ログイン画面のパスワード入力も打てなくなりうる。
	// 保存は止めず、確認してから保存する（CLI は --force、GUI はもう一度押す）
	bool StealsNormalInput(const Hotkey& hotkey);

	// Hotkey → "Ctrl+Alt+F9"。修飾キーはいつも Ctrl → Alt → Shift の順
	std::wstring FormatHotkey(const Hotkey& hotkey);
}
