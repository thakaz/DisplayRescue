#include "config/Hotkey.h"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <span>
#include <string>

#include "common/Common.h"

namespace {
	struct ModifierName {
		std::wstring_view name;
		UINT              flag;
	};

	// 書き出すときはこの順
	constexpr std::array kModifiers{
		ModifierName{ L"Ctrl",    MOD_CONTROL },
		ModifierName{ L"Alt",     MOD_ALT },
		ModifierName{ L"Shift",   MOD_SHIFT },
	};

	// 読むときだけ受け付ける別名（書き出さない）
	constexpr std::array kModifierAliases{
		ModifierName{ L"Control", MOD_CONTROL },
	};

	std::optional<UINT> FindModifier(std::wstring_view token) {
		for (const auto& table : { std::span<const ModifierName>{ kModifiers }, std::span<const ModifierName>{ kModifierAliases } }) {
			for (const auto& m : table) {
				if (rescue::EqualsIgnoreCase(m.name, token)) return m.flag;
			}
		}
		return std::nullopt;
	}

	struct KeyName {
		std::wstring_view name;
		UINT              vk;
	};

	// 名前で書けるキー（F1〜F24・A〜Z・0〜9 は別に扱う）。書き出すときはこの名前を使う。
	// 配列（日本語 / 英語）で意味が変わる記号のキー（VK_OEM_*）は名前を付けず、16 進で書く
	constexpr std::array kNamedKeys{
		KeyName{ L"Space",       VK_SPACE },
		KeyName{ L"Enter",       VK_RETURN },
		KeyName{ L"Tab",         VK_TAB },
		KeyName{ L"Backspace",   VK_BACK },
		KeyName{ L"Esc",         VK_ESCAPE },
		KeyName{ L"Insert",      VK_INSERT },
		KeyName{ L"Delete",      VK_DELETE },
		KeyName{ L"Home",        VK_HOME },
		KeyName{ L"End",         VK_END },
		KeyName{ L"PageUp",      VK_PRIOR },
		KeyName{ L"PageDown",    VK_NEXT },
		KeyName{ L"Left",        VK_LEFT },
		KeyName{ L"Up",          VK_UP },
		KeyName{ L"Right",       VK_RIGHT },
		KeyName{ L"Down",        VK_DOWN },
		KeyName{ L"Pause",       VK_PAUSE },
		KeyName{ L"ScrollLock",  VK_SCROLL },
		KeyName{ L"PrintScreen", VK_SNAPSHOT },
		KeyName{ L"CapsLock",    VK_CAPITAL },
		KeyName{ L"NumLock",     VK_NUMLOCK },
		KeyName{ L"Menu",        VK_APPS },
		KeyName{ L"Num0",        VK_NUMPAD0 },
		KeyName{ L"Num1",        VK_NUMPAD1 },
		KeyName{ L"Num2",        VK_NUMPAD2 },
		KeyName{ L"Num3",        VK_NUMPAD3 },
		KeyName{ L"Num4",        VK_NUMPAD4 },
		KeyName{ L"Num5",        VK_NUMPAD5 },
		KeyName{ L"Num6",        VK_NUMPAD6 },
		KeyName{ L"Num7",        VK_NUMPAD7 },
		KeyName{ L"Num8",        VK_NUMPAD8 },
		KeyName{ L"Num9",        VK_NUMPAD9 },
		KeyName{ L"NumMultiply", VK_MULTIPLY },
		KeyName{ L"NumAdd",      VK_ADD },
		KeyName{ L"NumSubtract", VK_SUBTRACT },
		KeyName{ L"NumDecimal",  VK_DECIMAL },
		KeyName{ L"NumDivide",   VK_DIVIDE },
	};

	// 読むときだけ受け付ける別名
	constexpr std::array kKeyAliases{
		KeyName{ L"Escape", VK_ESCAPE },
		KeyName{ L"Return", VK_RETURN },
		KeyName{ L"Ins",    VK_INSERT },
		KeyName{ L"Del",    VK_DELETE },
		KeyName{ L"PgUp",   VK_PRIOR },
		KeyName{ L"PgDn",   VK_NEXT },
		KeyName{ L"Apps",   VK_APPS },
	};

	bool IsFunctionKey(UINT vk) { return vk >= VK_F1 && vk <= VK_F24; }
	bool IsLetterOrDigit(UINT vk) { return (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'); }

	// "0xBA" → 0xBA。"0x" の後ろが 16 進の 1〜2 桁でなければ nullopt
	std::optional<UINT> ParseHexKey(std::wstring_view token) {
		if (token.size() < 3 || token.size() > 4 || token[0] != L'0' || (token[1] != L'x' && token[1] != L'X')) return std::nullopt;
		UINT value = 0;
		for (const wchar_t c : token.substr(2)) {
			int digit = 0;
			if (c >= L'0' && c <= L'9') digit = c - L'0';
			else if (c >= L'a' && c <= L'f') digit = c - L'a' + 10;
			else if (c >= L'A' && c <= L'F') digit = c - L'A' + 10;
			else return std::nullopt;
			value = value * 16 + static_cast<UINT>(digit);
		}
		return value;
	}

	// キー名 → 仮想キーコード
	std::optional<UINT> FindKey(std::wstring_view token) {
		if (token.size() == 1) {
			const wchar_t c = token[0];
			if (c >= L'0' && c <= L'9') return static_cast<UINT>(c);			// '0'〜'9' は VK もその文字コード
			if (c >= L'A' && c <= L'Z') return static_cast<UINT>(c);			// 'A'〜'Z' も同じ
			if (c >= L'a' && c <= L'z') return static_cast<UINT>(c - L'a' + L'A');
			return std::nullopt;
		}
		for (UINT n = 1; n <= 24; ++n) {
			if (rescue::EqualsIgnoreCase(token, std::format(L"F{}", n))) return VK_F1 + (n - 1);
		}
		for (const auto& table : { std::span<const KeyName>{ kNamedKeys }, std::span<const KeyName>{ kKeyAliases } }) {
			for (const auto& k : table) {
				if (rescue::EqualsIgnoreCase(k.name, token)) return k.vk;
			}
		}
		return ParseHexKey(token);
	}

	std::wstring NameOfKey(UINT vk) {
		if (IsFunctionKey(vk)) return std::format(L"F{}", vk - VK_F1 + 1);
		if (IsLetterOrDigit(vk)) return std::wstring(1, static_cast<wchar_t>(vk));
		if (const auto it = std::ranges::find(kNamedKeys, vk, &KeyName::vk); it != kNamedKeys.end()) return std::wstring{ it->name };
		return std::format(L"0x{:02X}", vk);
	}

	bool IsMouseButton(UINT vk) {
		switch (vk) {
		case VK_LBUTTON: case VK_RBUTTON: case VK_MBUTTON: case VK_XBUTTON1: case VK_XBUTTON2:
			return true;
		default:
			return false;
		}
	}
}

namespace rescue::config {

	bool IsModifierKey(UINT vk) {
		switch (vk) {
		case VK_SHIFT: case VK_CONTROL: case VK_MENU:
		case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
		case VK_LWIN: case VK_RWIN:
			return true;
		default:
			return false;
		}
	}

	std::optional<Hotkey> ParseHotkey(std::wstring_view text) {
		Hotkey hotkey{};
		bool haveKey = false;

		for (const auto part : text | std::views::split(L'+')) {
			const std::wstring_view token(part.begin(), part.end());
			if (token.empty()) return std::nullopt;		// "Ctrl++F9" や末尾の "+"
			if (haveKey) return std::nullopt;			// キー名の後ろにまだ何かある

			if (const auto modifier = FindModifier(token)) {
				if (hotkey.modifiers & *modifier) return std::nullopt;	// "Ctrl+Ctrl+F9"
				hotkey.modifiers |= *modifier;
				continue;
			}
			const auto vk = FindKey(token);
			if (!vk) return std::nullopt;				// 修飾キーでもキー名でもない
			hotkey.vk = *vk;
			haveKey = true;
		}

		if (!haveKey || !IsValidHotkey(hotkey)) return std::nullopt;
		return hotkey;
	}

	bool IsValidHotkey(const Hotkey& hotkey) {
		constexpr UINT kAllowed = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
		// キーにできないのは、マウスのボタンと修飾キーそのもの（修飾キーは modifiers で指定する）
		return (hotkey.modifiers & ~kAllowed) == 0
			&& hotkey.vk > 0 && hotkey.vk < 0xFF
			&& !IsMouseButton(hotkey.vk) && !IsModifierKey(hotkey.vk);
	}

	bool StealsNormalInput(const Hotkey& hotkey) {
		const bool hasCtrlOrAlt = (hotkey.modifiers & (MOD_CONTROL | MOD_ALT)) != 0;
		const bool isSpareKey = hotkey.vk >= VK_F13 && hotkey.vk <= VK_F24;	// 普段の入力でもアプリでもまず使われない
		return !hasCtrlOrAlt && !isSpareKey;
	}

	std::wstring FormatHotkey(const Hotkey& hotkey) {
		std::wstring text;
		for (const auto& m : kModifiers) {
			if (hotkey.modifiers & m.flag) {
				text += m.name;
				text += L'+';
			}
		}
		text += NameOfKey(hotkey.vk);
		return text;
	}
}
