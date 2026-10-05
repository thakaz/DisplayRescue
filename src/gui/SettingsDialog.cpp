#include "gui/SettingsDialog.h"

#include <windows.h>
#include <commctrl.h>       // HKM_SETHOTKEY / HKM_GETHOTKEY / HOTKEYF_*
#include <wil/resource.h>   // wil::unique_hhook
#include <wil/result.h>     // wil::ResultFromCaughtException

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ccd/Topology.h"
#include "common/Common.h"
#include "config/Settings.h"
#include "gui/resource.h"
#include "service/Installer.h"
#include "service/Service.h"

namespace {
	namespace ccd = rescue::ccd;
	namespace config = rescue::config;
	namespace service = rescue::service;

	// 警告の文字色（Windows の「エラー」系の赤に近い色）
	constexpr COLORREF kWarningColor = RGB(196, 43, 28);

	// キーボードフックが横取りしたキーを入力欄に入れるよう、ダイアログに頼むメッセージ。
	// wParam = 入力欄の添字（ccd::Topology）、lParam = 入力欄の値（HKM_SETHOTKEY の形）
	constexpr UINT WM_APP_CAPTURED_HOTKEY = WM_APP + 1;

	// ダイアログ 1 つ分の状態。DialogBoxParamW の lParam で渡し、DWLP_USER に覚えさせる
	struct DialogState {
		// 最後に読み込んだ・保存した割り当て。変わっていなければ保存しない。読み込めなかったときは nullopt（必ず保存する）
		std::optional<std::vector<config::Binding>> savedBindings;
		bool warning = false;			// メッセージ欄を赤字にするか
		bool riskConfirmed = false;		// 普段の入力を奪うキーの警告を出した後か（もう一度押せば保存する）
		bool olderThanInstalled = false;	// この設定画面がインストール済みの版より古いか（版の表示を赤字にし、再インストールの前に確認する）
		wil::unique_hhook keyboardHook;	// 割り当て済みのキーを待ち受け役より先に受け取るフック（KeyboardHook）
	};

	// フックの関数には引数で渡せないので、開いているダイアログをここに置く（ダイアログは同時に 1 つだけ）
	HWND g_dialog = nullptr;

	DialogState& State(HWND dialog) {
		return *reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
	}

	// 画面に出す操作の名前。ccd::AllTopologies() の順
	constexpr std::array<std::wstring_view, 4> kDisplayNames{ L"拡張", L"複製", L"メインのみ", L"サブのみ" };

	std::wstring_view DisplayName(ccd::Topology topology) {
		return kDisplayNames[static_cast<size_t>(topology)];
	}

	// ホットキー入力欄の修飾キー（HOTKEYF_*）と RegisterHotKey の修飾キー（MOD_*）は、同じ 3 つでもビットの並びが違う。
	//   HOTKEYF_SHIFT 0x01 / HOTKEYF_CONTROL 0x02 / HOTKEYF_ALT 0x04（ほかに HOTKEYF_EXT 0x08 = 拡張キー）
	//   MOD_ALT       0x01 / MOD_CONTROL     0x02 / MOD_SHIFT   0x04
	// そのまま代入すると Alt と Shift が入れ替わるので、必ずこの表で変換する
	constexpr std::array<std::pair<UINT, UINT>, 3> kModifierMap{ {
		{ HOTKEYF_SHIFT,   MOD_SHIFT },
		{ HOTKEYF_CONTROL, MOD_CONTROL },
		{ HOTKEYF_ALT,     MOD_ALT },
	} };

	// 「拡張キー」の仮想キー。テンキーと同じキーコードを持つ独立したキー（矢印・Home など）で、入力欄は HOTKEYF_EXT を付けて区別する。
	// RegisterHotKey は仮想キーで登録するので区別は要らないが、入力欄に戻すときに付けないと「Num 4」のようにテンキーの名前で表示される
	constexpr std::array kExtendedKeys{
		VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN, VK_INSERT, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT,
		VK_DIVIDE, VK_NUMLOCK, VK_APPS, VK_SNAPSHOT,
	};

	// 入力欄の値（下位バイト = 仮想キー、上位バイト = HOTKEYF_*）→ Hotkey。HOTKEYF_EXT は表示用の区別なので捨てる
	config::Hotkey FromControlValue(WORD value) {
		const UINT flags = HIBYTE(value);
		config::Hotkey hotkey{ .vk = LOBYTE(value) };
		for (const auto& [hotkeyFlag, modFlag] : kModifierMap) {
			if (flags & hotkeyFlag) hotkey.modifiers |= modFlag;
		}
		return hotkey;
	}

	// Hotkey → 入力欄の値
	WORD ToControlValue(const config::Hotkey& hotkey) {
		UINT flags = 0;
		for (const auto& [hotkeyFlag, modFlag] : kModifierMap) {
			if (hotkey.modifiers & modFlag) flags |= hotkeyFlag;
		}
		if (std::ranges::find(kExtendedKeys, static_cast<int>(hotkey.vk)) != kExtendedKeys.end()) flags |= HOTKEYF_EXT;
		return MAKEWORD(hotkey.vk, flags);
	}

	HWND HotkeyControl(HWND dialog, ccd::Topology topology) {
		return GetDlgItem(dialog, IDC_HOTKEY_FIRST + static_cast<int>(topology));
	}

	WORD ControlValue(HWND dialog, ccd::Topology topology) {
		return static_cast<WORD>(SendMessageW(HotkeyControl(dialog, topology), HKM_GETHOTKEY, 0, 0));
	}

	// メッセージ欄に出す。warning なら赤字（色は WM_CTLCOLORSTATIC で付ける）
	void ShowMessage(HWND dialog, std::wstring_view text, bool warning = false) {
		State(dialog).warning = warning;
		SetDlgItemTextW(dialog, IDC_MESSAGE, std::wstring{ text }.c_str());
		InvalidateRect(GetDlgItem(dialog, IDC_MESSAGE), nullptr, TRUE);
	}

	// サービスの状態と、インストール・アンインストールのボタンを今の状態に合わせる
	void ShowServiceState(HWND dialog) {
		const auto state = service::QueryServiceState();
		std::wstring_view status;
		switch (state) {
		case service::ServiceState::Running:      status = L"実行中です。割り当てたキーで切り替えられます。"; break;
		case service::ServiceState::Stopped:      status = L"停止しています。キーは効きません(再インストールすると起動します)。"; break;
		case service::ServiceState::Pending:      status = L"起動または停止の途中です。"; break;
		case service::ServiceState::NotInstalled: status = L"インストールされていません。「インストール」でキーが使えるようになります。"; break;
		case service::ServiceState::Unknown:      status = L"状態を取得できませんでした。"; break;
		}
		SetDlgItemTextW(dialog, IDC_SERVICE_STATUS, std::wstring{ status }.c_str());

		// 版。古い zip の設定画面で再インストールすると古い版に戻るので、そのときは赤字で知らせる
		const auto current = rescue::CurrentVersion();
		const auto installedVersion = service::InstalledVersion();
		auto& dialogState = State(dialog);
		dialogState.olderThanInstalled = installedVersion && current < *installedVersion;
		std::wstring versionText = installedVersion
			? std::format(L"インストール済みの版: {} ／ この設定画面の版: {}", installedVersion->ToString(), current.ToString())
			: std::format(L"この設定画面の版: {}", current.ToString());
		if (dialogState.olderThanInstalled) versionText += L"\n(この設定画面のほうが古いため、再インストールすると古い版に戻ります)";
		SetDlgItemTextW(dialog, IDC_VERSION, versionText.c_str());
		InvalidateRect(GetDlgItem(dialog, IDC_VERSION), nullptr, TRUE);

		// インストール済みなら「再インストール」（新しい版の zip を展開して、その設定画面から上書きするため）
		const bool installed = state != service::ServiceState::NotInstalled;
		SetDlgItemTextW(dialog, IDC_INSTALL, installed ? L"再インストール" : L"インストール");
		EnableWindow(GetDlgItem(dialog, IDC_UNINSTALL), installed);
	}

	// 今の割り当てを入力欄に出す
	void LoadBindingsToDialog(HWND dialog) {
		try {
			const auto bindings = config::LoadBindings();
			State(dialog).savedBindings = bindings;
			for (const auto topology : ccd::AllTopologies()) {
				const auto it = std::ranges::find(bindings, topology, &config::Binding::topology);
				const WORD value = it != bindings.end() ? ToControlValue(it->hotkey) : 0;
				SendMessageW(HotkeyControl(dialog, topology), HKM_SETHOTKEY, value, 0);
			}
		}
		catch (...) {
			ShowMessage(dialog, std::format(L"設定を読み込めませんでした: {}", rescue::DescribeHresult(wil::ResultFromCaughtException())), true);
		}
	}

	// 入力欄の問題 1 つ（保存できないもの）
	struct Problem {
		ccd::Topology	topology;
		std::wstring	message;
	};

	// 入力欄を全部見て、割り当てを集める。
	//   problem: 保存できない入力（成り立たない・重なっている）の最初の 1 つ
	//   risky:   保存はできるが確認が要るもの（Ctrl も Alt も無く、普段の入力を奪う）
	struct Collected {
		std::vector<config::Binding>	bindings;
		std::optional<Problem>			problem;
		std::vector<config::Binding>	risky;
	};

	Collected Collect(HWND dialog) {
		Collected result;
		for (const auto topology : ccd::AllTopologies()) {
			const WORD value = ControlValue(dialog, topology);
			if (value == 0) continue;	// 空欄 = 割り当てなし

			const auto name = DisplayName(topology);
			const auto hotkey = FromControlValue(value);
			const auto fail = [&](std::wstring message) {
				if (!result.problem) result.problem = Problem{ topology, std::move(message) };
			};

			if (!config::IsValidHotkey(hotkey)) {
				fail(std::format(L"「{}」のキーはホットキーにできません。", name));
				continue;
			}
			if (const auto same = std::ranges::find(result.bindings, hotkey, &config::Binding::hotkey); same != result.bindings.end()) {
				fail(std::format(L"「{}」と「{}」が同じキー({})です。", DisplayName(same->topology), name, config::FormatHotkey(hotkey)));
				continue;
			}
			result.bindings.push_back({ topology, hotkey });
			if (config::StealsNormalInput(hotkey)) result.risky.push_back({ topology, hotkey });
		}
		return result;
	}

	// 確認が要るキーの説明。「「拡張」の A」のように並べる
	std::wstring DescribeRisky(const std::vector<config::Binding>& risky) {
		std::wstring list;
		for (const auto& b : risky) {
			if (!list.empty()) list += L"、";
			list += std::format(L"「{}」の{}", DisplayName(b.topology), config::FormatHotkey(b.hotkey));
		}
		return list;
	}

	// 入力が変わったら、すぐ確かめてメッセージ欄に出す
	void OnHotkeyChanged(HWND dialog, ccd::Topology changed) {
		State(dialog).riskConfirmed = false;	// 入力が変わったら確認はやり直し

		const auto collected = Collect(dialog);
		if (collected.problem) {
			ShowMessage(dialog, collected.problem->message, true);
			return;
		}
		const WORD value = ControlValue(dialog, changed);
		if (value == 0) {
			ShowMessage(dialog, std::format(L"「{}」は割り当てなしになります。", DisplayName(changed)));
			return;
		}
		const auto hotkey = FromControlValue(value);
		if (config::StealsNormalInput(hotkey)) {
			ShowMessage(dialog, std::format(L"{}にはCtrlもAltも付いていないため、このキーはWindows全体で使えなくなります。保存するときに確認します。",
				config::FormatHotkey(hotkey)), true);
			return;
		}
		ShowMessage(dialog, std::format(L"「{}」を{}にします。「OK」か「適用」で保存します。", DisplayName(changed), config::FormatHotkey(hotkey)));
	}

	// 入力欄を既定の割り当て（Ctrl+Alt+F8〜F11）に戻す。「外す」と同じく、保存は OK か適用を押したとき
	void OnResetDefaults(HWND dialog) {
		const auto defaults = config::DefaultBindings();
		for (const auto topology : ccd::AllTopologies()) {
			const auto it = std::ranges::find(defaults, topology, &config::Binding::topology);
			const WORD value = it != defaults.end() ? ToControlValue(it->hotkey) : 0;
			SendMessageW(HotkeyControl(dialog, topology), HKM_SETHOTKEY, value, 0);
		}
		State(dialog).riskConfirmed = false;	// 入力が変わったので確認はやり直し（既定のキーに確認の要るものは無い）
		ShowMessage(dialog, State(dialog).savedBindings == defaults
			? L"既定の割り当てにしました(今の設定と同じです)。"
			: L"既定の割り当てにしました。「OK」か「適用」で保存します。");
	}

	// 保存してサービスに反映する。保存できたら true
	bool Save(HWND dialog) {
		auto& state = State(dialog);
		const auto collected = Collect(dialog);
		if (collected.problem) {
			ShowMessage(dialog, collected.problem->message, true);
			MessageBeep(MB_ICONWARNING);
			SetFocus(HotkeyControl(dialog, collected.problem->topology));
			return false;
		}
		if (state.savedBindings == collected.bindings) {
			ShowMessage(dialog, L"変更はありません。");
			return true;
		}
		// 止めはしないが、知らずに自分を締め出さないよう、1 度目は警告して止まる。もう一度押されたら保存する
		if (!collected.risky.empty() && !state.riskConfirmed) {
			ShowMessage(dialog, std::format(
				L"{}はWindows全体で使えなくなり、ログイン画面のパスワードも打てなくなるおそれがあります。それでよければ、もう一度押すと保存します。",
				DescribeRisky(collected.risky)), true);
			MessageBeep(MB_ICONWARNING);
			state.riskConfirmed = true;
			return false;
		}

		try {
			// 重なりは Collect で確かめ済みなので、ここで返ってくることはない
			if (config::ReplaceBindings(collected.bindings)) return false;
			state.savedBindings = collected.bindings;
			state.riskConfirmed = false;
		}
		catch (...) {
			ShowMessage(dialog, std::format(L"保存できませんでした: {}", rescue::DescribeHresult(wil::ResultFromCaughtException())), true);
			MessageBeep(MB_ICONWARNING);
			return false;
		}

		// 待ち受け役は起動時にしか割り当てを読まないので、サービスに置き直してもらう
		const bool reloaded = service::RequestServiceReload();
		ShowServiceState(dialog);
		ShowMessage(dialog, reloaded
			? L"保存しました。新しいキーで切り替えられます。"
			: L"保存しました。サービスが動き出すと有効になります。");
		return true;
	}

	// インストール・アンインストールを実行する。数秒かかる（サービスの停止を待つ）ので、その間は砂時計にして途中経過を出す。
	// 別のスレッドにはしない（その間に設定を触られても困るので、ダイアログが止まっていてよい）
	void RunServiceSetup(HWND dialog, std::wstring_view what, void (*setup)(const service::Progress&)) {
		const HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
		const auto progress = [&](std::wstring_view line) {
			ShowMessage(dialog, line);
			UpdateWindow(GetDlgItem(dialog, IDC_MESSAGE));	// メッセージループが回らない間も、すぐ描かせる
		};
		try {
			setup(progress);
		}
		catch (...) {
			ShowMessage(dialog, std::format(L"{}に失敗しました: {}", what, rescue::DescribeHresult(wil::ResultFromCaughtException())), true);
			MessageBeep(MB_ICONWARNING);
		}
		SetCursor(previous);
		ShowServiceState(dialog);
	}

	// 取り返しのつかない操作の前に「はい / いいえ」で確かめる。既定は「いいえ」（Enter の押し間違いで進まないように）
	bool Confirm(HWND dialog, const wchar_t* title, const wchar_t* text) {
		return MessageBoxW(dialog, text, title, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
	}

	void OnInstall(HWND dialog) {
		if (State(dialog).olderThanInstalled && !Confirm(dialog, L"DisplayRescueの再インストール",
			L"インストール済みの版のほうが新しいです。再インストールすると、この設定画面の古い版に戻ります。\n続けますか？")) {
			return;
		}
		RunServiceSetup(dialog, L"インストール", service::Install);
	}

	void OnUninstall(HWND dialog) {
		if (!Confirm(dialog, L"DisplayRescueのアンインストール",
			L"サービスとキーの設定を削除します。アンインストールすると、キーで画面構成を戻せなくなります。\n続けますか？")) {
			return;
		}
		RunServiceSetup(dialog, L"アンインストール", service::Uninstall);
		LoadBindingsToDialog(dialog);	// キーの設定を消したので、入力欄を既定の割り当てに戻す
	}

	// タイトルバーとタスクバーのアイコン。DPI に合った大きさで読み込む
	void SetWindowIcons(HWND dialog) {
		const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE));
		const UINT dpi = GetDpiForWindow(dialog);
		for (const auto kind : { ICON_SMALL, ICON_BIG }) {
			const int cx = GetSystemMetricsForDpi(kind == ICON_SMALL ? SM_CXSMICON : SM_CXICON, dpi);
			const int cy = GetSystemMetricsForDpi(kind == ICON_SMALL ? SM_CYSMICON : SM_CYICON, dpi);
			// LR_SHARED のアイコンはシステムが所有するため DestroyIcon は不要
			const auto icon = LoadImageW(instance, MAKEINTRESOURCEW(IDI_DISPLAY_RESCUE), IMAGE_ICON, cx, cy, LR_SHARED);
			SendMessageW(dialog, WM_SETICON, kind, reinterpret_cast<LPARAM>(icon));
		}
	}

	void OnInitDialog(HWND dialog, LPARAM lParam) {
		SetWindowLongPtrW(dialog, DWLP_USER, lParam);
		SetWindowIcons(dialog);
		LoadBindingsToDialog(dialog);
		ShowServiceState(dialog);
		if (!State(dialog).warning) ShowMessage(dialog, L"入力欄を選んで、割り当てたいキーを押してください。");
	}

	// 各行の操作のアイコン（owner draw の Static）を描く
	void OnDrawItem(HWND dialog, const DRAWITEMSTRUCT& item) {
		if (item.CtlID < IDC_MODE_FIRST || item.CtlID >= IDC_MODE_FIRST + ccd::AllTopologies().size()) return;
		FillRect(item.hDC, &item.rcItem, GetSysColorBrush(COLOR_BTNFACE));
		const int width = item.rcItem.right - item.rcItem.left;
		const int height = item.rcItem.bottom - item.rcItem.top;
		const int size = (std::min)(width, height);
		const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE));
		const auto icon = static_cast<HICON>(LoadImageW(instance,
			MAKEINTRESOURCEW(IDI_MODE_FIRST + item.CtlID - IDC_MODE_FIRST), IMAGE_ICON, size, size, LR_SHARED));
		DrawIconEx(item.hDC, item.rcItem.left + (width - size) / 2,
			item.rcItem.top + (height - size) / 2, icon, size, size, 0, nullptr, DI_NORMAL);
	}

	std::optional<ccd::Topology> TopologyFromId(int id, int first) {
		const int index = id - first;
		if (index < 0 || index >= static_cast<int>(ccd::AllTopologies().size())) return std::nullopt;
		return static_cast<ccd::Topology>(index);
	}

	// 今押されている修飾キー（MOD_*）
	UINT CurrentModifiers() {
		UINT modifiers = 0;
		if (GetAsyncKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
		if (GetAsyncKeyState(VK_MENU) & 0x8000)    modifiers |= MOD_ALT;
		if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   modifiers |= MOD_SHIFT;
		return modifiers;
	}

	// 低レベルのキーボードフック。キーが押されるたびに、どのアプリに届くよりも先に呼ばれる。
	// 入力欄でキーを押すと、そのキーが割り当て済みなら、サービスの待ち受け役（RegisterHotKey）が横取りして画面が切り替わってしまい、
	// 入力欄にも届かない。RegisterHotKey の判定はこのフックの後なので、ここで握りつぶせば待ち受け役は反応しない。
	// 握りつぶすのは「このダイアログが前面・キーの入力欄にフォーカス・割り当て済みの組み合わせ」のときだけ。
	// それ以外のキーはそのまま流す（入力欄が修飾キーの途中経過を表示し、Tab で移動できるように）。
	// フックはこのプロセスのものなので、ダイアログを閉じる・プロセスが落ちると必ず外れる（待ち受けが止まったままにはならない）
	LRESULT CALLBACK KeyboardHook(int code, WPARAM wParam, LPARAM lParam) {
		if (code == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
			&& g_dialog && GetForegroundWindow() == g_dialog) {
			const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
			const auto topology = TopologyFromId(GetDlgCtrlID(GetFocus()), IDC_HOTKEY_FIRST);
			if (topology && !config::IsModifierKey(key.vkCode)) {
				const config::Hotkey pressed{ .modifiers = CurrentModifiers(), .vk = key.vkCode };
				const auto& saved = State(g_dialog).savedBindings;
				if (saved && std::ranges::find(*saved, pressed, &config::Binding::hotkey) != saved->end()) {
					// フックの中は手早く済ませる決まり（遅いと Windows にフックを外される）ので、入力欄の更新はダイアログに任せる
					PostMessageW(g_dialog, WM_APP_CAPTURED_HOTKEY, static_cast<WPARAM>(*topology), ToControlValue(pressed));
					return 1;	// 0 以外 = このキーはここで終わり（待ち受け役にも入力欄にも届かない）
				}
			}
		}
		return CallNextHookEx(nullptr, code, wParam, lParam);
	}

	// ダイアログに届くメッセージを処理する。処理したら TRUE、既定の処理に任せるなら FALSE
	INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
		switch (message) {
		case WM_INITDIALOG:
			OnInitDialog(dialog, lParam);
			g_dialog = dialog;
			// 付けられなくても設定はできる（割り当て済みのキーを入力欄で押すと画面が切り替わるだけ）ので、失敗は無視する
			State(dialog).keyboardHook.reset(SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, GetModuleHandleW(nullptr), 0));
			return TRUE;	// TRUE = 最初のコントロールにフォーカスを置いてもらう

		case WM_DESTROY:
			State(dialog).keyboardHook.reset();
			g_dialog = nullptr;
			return FALSE;

		// フックが握りつぶしたキーを、押された入力欄に入れる（入力欄で普通に押したときと同じ扱いにする）
		case WM_APP_CAPTURED_HOTKEY: {
			const auto topology = static_cast<ccd::Topology>(wParam);
			SendMessageW(HotkeyControl(dialog, topology), HKM_SETHOTKEY, static_cast<WPARAM>(lParam), 0);
			OnHotkeyChanged(dialog, topology);
			return TRUE;
		}

		case WM_DRAWITEM:
			OnDrawItem(dialog, *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
			return TRUE;

		// Static が描く前に色を聞いてくる。メッセージ欄の警告と、古い版の知らせだけ赤字にする（背景はダイアログと同じ）
		case WM_CTLCOLORSTATIC: {
			if (GetWindowLongPtrW(dialog, DWLP_USER) == 0) return FALSE;	// WM_INITDIALOG の前（状態をまだ覚えていない）
			const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
			const auto& state = State(dialog);
			const bool red = (id == IDC_MESSAGE && state.warning) || (id == IDC_VERSION && state.olderThanInstalled);
			if (!red) return FALSE;
			const auto dc = reinterpret_cast<HDC>(wParam);
			SetTextColor(dc, kWarningColor);
			SetBkMode(dc, TRANSPARENT);
			return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
		}

		case WM_COMMAND: {
			const int id = LOWORD(wParam);
			// ホットキー入力欄は、キーが押されて値が変わると EN_CHANGE で知らせてくる
			if (const auto topology = TopologyFromId(id, IDC_HOTKEY_FIRST); topology && HIWORD(wParam) == EN_CHANGE) {
				OnHotkeyChanged(dialog, *topology);
				return TRUE;
			}
			if (const auto topology = TopologyFromId(id, IDC_CLEAR_FIRST)) {
				SendMessageW(HotkeyControl(dialog, *topology), HKM_SETHOTKEY, 0, 0);
				OnHotkeyChanged(dialog, *topology);
				return TRUE;
			}
			switch (id) {
			case IDOK:
				if (Save(dialog)) EndDialog(dialog, IDOK);
				return TRUE;
			case IDC_APPLY:
				Save(dialog);
				return TRUE;
			case IDC_INSTALL:
				OnInstall(dialog);
				return TRUE;
			case IDC_UNINSTALL:
				OnUninstall(dialog);
				return TRUE;
			case IDC_RESET_DEFAULTS:
				OnResetDefaults(dialog);
				return TRUE;
			case IDCANCEL:	// キャンセルボタン・Esc・閉じるボタン（×）のどれでも来る
				EndDialog(dialog, IDCANCEL);
				return TRUE;
			}
			break;
		}
		}
		return FALSE;
	}
}

void rescue::gui::ShowSettingsDialog(HINSTANCE instance) {
	DialogState state;
	DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, DialogProc, reinterpret_cast<LPARAM>(&state));
}
