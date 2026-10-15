// サービスが置きっぱなしにするマスコット。
// ウィンドウは最前面に表示し、クリックを透過してフォーカスを取得しない。サービスから停止を指示されるまで、まばたき・モーション・クロスフェードを描画し続ける。
// モデルとモーションは ProgramData の変換した形式（AssetFile.cpp）から読む。
// 見えていない側（ロックしていないときのパスワード欄用など）は Present が DXGI_STATUS_OCCLUDED を返すので、少し休みながら回る。
// 再生するモーションと時刻は、OS の時計と日付をシードとして MotionSchedule が決める（複数のマスコットで同じ結果になる）。
// 見えていない間はほぼ透明にしておき、見え始めたら appearSeconds かけて出てくる（Canvas::SetOpacity）
// ロック画像用（Default デスクトップ）は、ロック中だけ見せ、普段のデスクトップでは窓を隠す（options.showOnDesktop なら隠さない）。
//   ロック中かは SessionLockWatcher。窓に届く WM_WTSSESSION_CHANGE を WindowProc から渡す。
//   パスワード欄用（Winlogon デスクトップ）も見張る（Winlogon は UAC の確認画面でも前に出るので、ロック中だけ見せる）
// options.edit なら調整用の窓（画面いっぱい）: クリックとキーを受け、操作は PlacementEditor。Enter で設定に保存して終わる
// 拡張・複製などで画面の構成が変わったら、メインのモニターを読み直して窓を配置し直す。
//   知らせ（WM_DISPLAYCHANGE / WM_SETTINGCHANGE）と、1 秒ごとのモニタの一覧の見比べ（Monitors.cpp）の両方で気づく
// ロック中は、各モニタに画面切り替えキーの案内を表示する（ShortcutCards.cpp）。表示と非表示はマスコットに合わせる
// options.previewMotion なら、普段の窓（クリック素通し）のままデスクトップに出し、
//   予定表の代わりに「待機 → そのモーション → 待機」を 1 回再生して終了する。ロック状態の監視とキーの案内は使わない
#include "mascot/Mascot.h"

#include <windows.h>
#include <DirectXMath.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <vector>

#include "ccd/Topology.h"
#include "common/Common.h"
#include "common/Log.h"
#include "config/Hotkey.h"
#include "config/MascotSettings.h"
#include "config/Settings.h"
#include "mascot/AssetFile.h"
#include "mascot/Canvas.h"
#include "mascot/Model.h"
#include "mascot/Monitors.h"
#include "mascot/MotionSchedule.h"
#include "mascot/PlacementEditor.h"
#include "mascot/SessionLockWatcher.h"
#include "mascot/ShortcutCards.h"

namespace {
	using namespace DirectX;

	constexpr wchar_t kClassName[] = L"DisplayRescueMascot";
	constexpr UINT_PTR kTimerRaise = 1;			// 1 秒ごとに最前面にし直す（ロック画面の部品に負けないように）
	constexpr UINT kRaiseIntervalMs = 1000;
	constexpr float kBlinkTime = 0.16f;

	constexpr float kHiddenOpacity = 1.0f / 255.0f;	// 見えていない間の不透明度

	// UTC の今。day は 1601 年 1 月 1 日からの日数（乱数の種にする）、secondsOfDay はその日の何秒目か、absolute は 1601 年からの秒。
	// FILETIME は 100 ナノ秒単位。double は 1601 年からの秒でも小数 6 桁ほど持てるので、フレームの間隔には十分
	struct Clock {
		std::uint64_t day;
		double secondsOfDay;
		double absolute;
	};
	Clock ClockNow() {
		FILETIME time{};
		GetSystemTimePreciseAsFileTime(&time);
		const std::uint64_t ticks = (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
		constexpr std::uint64_t kTicksPerSecond = 10'000'000;
		constexpr std::uint64_t kTicksPerDay = kTicksPerSecond * 60 * 60 * 24;
		return {
			ticks / kTicksPerDay,
			static_cast<double>(ticks % kTicksPerDay) / kTicksPerSecond,
			static_cast<double>(ticks) / kTicksPerSecond,
		};
	}

	// WindowProc とループが一緒に使うもの。WindowProc は OS から呼ばれるので引数では渡せないが、
	// 窓を作るときに lpParam で渡し、WM_NCCREATE で窓の GWLP_USERDATA に覚えさせれば、メッセージのたびに取り出せる
	struct WindowState {
		rescue::mascot::SessionLockWatcher* lockWatcher = nullptr;	// ロックを見張っている間だけ指す
		rescue::mascot::PlacementEditor* editor = nullptr;			// 調整している間だけ指す
		bool layoutChanged = false;	// 画面の構成が変わったと窓に知らせが来た（WM_DISPLAYCHANGE など）。ループが見て配置し直し、false に戻す
	};
	// 知らせが来なくても（別のデスクトップにいるときなど）、この間隔でモニタの一覧を調べ直す
	constexpr auto kLayoutCheckInterval = std::chrono::seconds{ 1 };

	// 枠の無い形。利用者が決めるのは「キャラの中心が画面のどこか」「背の高さが画面の何割か」「向き」だけ。
	// カメラはいつも同じ（原点から kCameraDistance 離れて正面を見る）で、モデルの 1 単位が何ピクセルになるか（pixelsPerUnit）で大きさを決める。
	// 窓はキャラを囲む範囲だけにして（画面いっぱいだと重い）、射影は「画面全体で見たときの、その窓の部分だけ」にする（XMMatrixPerspectiveOffCenterLH）。
	// こうすると、窓の大きさや位置が変わってもキャラの写り方は変わらない（調整中の画面いっぱいの窓と、普段の小さい窓で同じに見える）
	constexpr float kCameraDistance = 3.4f;
	constexpr float kNear = 0.05f;
	constexpr float kFar = 100.0f;

	// モデルの 1 単位（原点の深さで）が何ピクセルか。モデルは背の高さがだいたい 2 にそろえてあるので、2 単位 = scale × 画面の高さ
	float PixelsPerUnit(const RECT& screenArea, float scale) {
		return scale * static_cast<float>(screenArea.bottom - screenArea.top) / 2.0f;
	}

	// 窓 window（画面の座標）に、原点がちょうど (centerX, centerY)（画面の座標）に写るような射影。
	// 近くの面（kNear）の上で 1 ピクセルが何単位かを k にして、窓の四辺を中心からの距離で表す（y は上が +）
	XMMATRIX ProjectionFor(const RECT& window, float centerX, float centerY, float pixelsPerUnit) {
		const float k = kNear / (kCameraDistance * pixelsPerUnit);
		return XMMatrixPerspectiveOffCenterLH(
			(window.left - centerX) * k, (window.right - centerX) * k,
			(centerY - window.bottom) * k, (centerY - window.top) * k,
			kNear, kFar);
	}

	// 普段の窓: キャラを囲む球（半径 radius。どう回しても入る）を写した四角に少し余裕を足し、画面と重なる部分だけ。
	// 球は遠近で少し大きく写るので、接線の角度で広げる（r / √(1 - (r/D)²)）。重ならなければ（画面の外）、1 ピクセルの窓
	RECT MascotRect(const RECT& screenArea, float centerX, float centerY, float pixelsPerUnit, float radius) {
		const float r = (std::min)(radius, kCameraDistance * 0.9f);
		const float half = r / std::sqrt(1.0f - (r / kCameraDistance) * (r / kCameraDistance)) * pixelsPerUnit * 1.05f + 2.0f;
		const RECT bounds{
			static_cast<LONG>(std::floor(centerX - half)), static_cast<LONG>(std::floor(centerY - half)),
			static_cast<LONG>(std::ceil(centerX + half)), static_cast<LONG>(std::ceil(centerY + half)) };
		RECT clipped{};
		if (!IntersectRect(&clipped, &bounds, &screenArea)) clipped = { screenArea.left, screenArea.top, screenArea.left + 1, screenArea.top + 1 };
		return clipped;
	}

	// このスレッドのデスクトップが Winlogon（パスワード欄・ログイン画面）か。名前は GetUserObjectInformationW の UOI_NAME
	bool IsOnWinlogonDesktop() {
		wchar_t name[64]{};
		DWORD needed = 0;
		if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, name, sizeof(name), &needed)) return false;
		return rescue::EqualsIgnoreCase(name, L"Winlogon");
	}

	void Report(std::wstring_view message) {
		rescue::Log(std::format(L"[mascot pid {}] {}", GetCurrentProcessId(), message));
	}

	// 画面に出す操作の名前（設定アプリ・README と同じ言葉）
	std::wstring_view ActionName(rescue::ccd::Topology topology) {
		switch (topology) {
		case rescue::ccd::Topology::Extend:		return L"拡張";
		case rescue::ccd::Topology::Clone:		return L"複製";
		case rescue::ccd::Topology::Internal:	return L"メインのみ";
		case rescue::ccd::Topology::External:	return L"サブのみ";
		}
		return L"?";
	}

	// 現在の割り当てを案内の各行にする。順番は拡張・複製・メインのみ・サブのみ（AllTopologies の順）
	std::vector<rescue::mascot::ShortcutLine> ShortcutLines() {
		std::vector<rescue::config::Binding> bindings = rescue::config::LoadBindings();
		std::ranges::sort(bindings, {}, [](const rescue::config::Binding& b) { return static_cast<int>(b.topology); });
		std::vector<rescue::mascot::ShortcutLine> lines;
		for (const rescue::config::Binding& binding : bindings) {
			lines.push_back({ rescue::config::FormatHotkey(binding.hotkey), std::wstring{ ActionName(binding.topology) } });
		}
		return lines;
	}

	// モーションのフォルダの .motion（変換したもの）を名前の順に（サブフォルダも）。フォルダが無ければ空
	std::vector<std::filesystem::path> FindMotions(const std::filesystem::path& folder) {
		std::vector<std::filesystem::path> motions;
		std::error_code error;
		for (const auto& entry : std::filesystem::recursive_directory_iterator{ folder, error }) {
			if (rescue::EqualsIgnoreCase(entry.path().extension().wstring(), L".motion")) motions.push_back(entry.path());
		}
		std::ranges::sort(motions);
		return motions;
	}

	LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
		// 作られた最初に、CreateWindowExW の lpParam（WindowState のアドレス）を窓に覚えさせる
		if (message == WM_NCCREATE) {
			const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
		}
		auto* const state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (!state) return DefWindowProcW(hwnd, message, wParam, lParam);	// WM_NCCREATE より前に来るもの

		// 調整中は、まず PlacementEditor に渡す。処理した（戻り値がある）ならそれを返す
		if (state->editor) {
			if (const auto result = state->editor->HandleMessage(hwnd, message, wParam, lParam)) return *result;
		}
		switch (message) {
		case WM_NCHITTEST:		if (!state->editor) return HTTRANSPARENT; break;	// マウスは下のウィンドウに回す（調整中は受ける）
		case WM_MOUSEACTIVATE:	if (!state->editor) return MA_NOACTIVATE; break;	// クリックされても前面にならない（調整中はなる。キーを受けるため）
		case WM_TIMER:
			// 隠しているときは最前面にし直さない（SWP_SHOWWINDOW で出てきてしまう）
			if (wParam == kTimerRaise && IsWindowVisible(hwnd)) SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
			return 0;
		case WM_WTSSESSION_CHANGE:	// ロック・解除など。wParam が種類
			if (state->lockWatcher) state->lockWatcher->OnSessionChange(wParam);
			return 0;
		case WM_DISPLAYCHANGE:		// 解像度やモニタの数が変わった（拡張・複製の切り替えも）
			state->layoutChanged = true;
			return 0;
		case WM_SETTINGCHANGE:		// 画面が変わった（タスクバーの位置など）。ほかの設定の変化は DefWindowProcW に
			if (wParam == SPI_SETWORKAREA) state->layoutChanged = true;
			break;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(hwnd, message, wParam, lParam);
	}

	// ウィンドウクラス（WindowProc）を登録する。キャラクターのウィンドウと、キーの案内だけを表示するときの通知用ウィンドウで共用する
	void RegisterWindowClass(HCURSOR cursor) {
		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof(windowClass);
		windowClass.lpfnWndProc = WindowProc;
		windowClass.hInstance = GetModuleHandleW(nullptr);
		windowClass.hCursor = cursor;
		windowClass.lpszClassName = kClassName;
		THROW_LAST_ERROR_IF(RegisterClassExW(&windowClass) == 0);
	}

	// 各モニタにキーの案内を作る。割り当てがない場合や作成に失敗した場合は nullopt（キャラクターはそのまま表示する）
	std::optional<rescue::mascot::ShortcutCards> MakeCards(const std::vector<rescue::mascot::MonitorArea>& monitors) {
		try {
			auto lines = ShortcutLines();
			if (lines.empty()) return std::nullopt;
			std::optional<rescue::mascot::ShortcutCards> cards;
			cards.emplace(std::move(lines));	// コピーできないクラスなので、optional の中に直接作る
			cards->Place(monitors);
			return cards;
		}
		catch (...) {
			Report(std::format(L"画面切り替えキーの案内を表示できませんでした: {}", rescue::DescribeHresult(wil::ResultFromCaughtException())));
			return std::nullopt;
		}
	}

	// 知らせが来たか、前に調べてから kLayoutCheckInterval たったら、モニタの一覧を調べ直す。
	// checked = 確認済み（キーの案内を最前面に戻す合図にも使う）、changed = 一覧が変わった（monitors は更新済み）
	struct LayoutCheck {
		bool checked = false;
		bool changed = false;
	};
	LayoutCheck CheckLayout(WindowState& state, std::vector<rescue::mascot::MonitorArea>& monitors, std::chrono::steady_clock::time_point& lastCheck) {
		const auto now = std::chrono::steady_clock::now();
		if (!state.layoutChanged && now - lastCheck < kLayoutCheckInterval) return {};
		state.layoutChanged = false;
		lastCheck = now;
		std::vector<rescue::mascot::MonitorArea> current = rescue::mascot::EnumerateMonitors();
		if (current == monitors) return { true, false };
		monitors = std::move(current);
		return { true, true };
	}

	// モデルに足したモーション。番号は Model::AddMotion の順（フォルダのモーション → 最後に待機モーション）
	struct LoadedMotions {
		std::vector<float> durations;			// 番号ごとの長さ（予定表に渡す）
		std::optional<std::size_t> idleIndex;	// 待機モーションの番号。無ければ nullopt
		std::size_t otherCount = 0;				// 待機モーションを除いた本数
	};

	// モーションを全部読んでモデルに足す（長さが分からないと予定表を作れないので、最初に全部）。読めないものは飛ばす。
	// 待機モーション（mascot-idle.motion。モーションのフォルダの外）があれば最後に足す。乱数で選ぶのは予定表がそれ以外から
	LoadedMotions AddMotions(rescue::mascot::Model& model, const std::vector<std::filesystem::path>& motions, const std::filesystem::path& idlePath) {
		LoadedMotions loaded;
		for (const std::filesystem::path& motion : motions) {
			try {
				model.AddMotion(rescue::mascot::LoadMotionAsset(motion), motion.filename().wstring());
				loaded.durations.push_back(model.MotionDuration(loaded.durations.size()));
			}
			catch (...) {
				Report(std::format(L"モーションを読めませんでした: {}: {}", motion.filename().wstring(), rescue::DescribeHresult(wil::ResultFromCaughtException())));
			}
		}
		loaded.otherCount = loaded.durations.size();
		std::error_code error;
		if (std::filesystem::is_regular_file(idlePath, error)) {
			try {
				model.AddMotion(rescue::mascot::LoadMotionAsset(idlePath), idlePath.filename().wstring());
				loaded.idleIndex = loaded.durations.size();
				loaded.durations.push_back(model.MotionDuration(loaded.durations.size()));
			}
			catch (...) {
				Report(std::format(L"待機モーションを読めませんでした: {}", rescue::DescribeHresult(wil::ResultFromCaughtException())));
			}
		}
		return loaded;
	}

	// 普段の再生のしかた。スケジュール（MotionSchedule）で「今どのモーションか、いつ始まったか」を決め、変わったら再生する。日が変わったら、その日の種で作り直す。
	// 始まった時刻を Draw の seconds の物差し（epoch からの秒）に直して渡すので、途中から起動しても、もう一方のマスコットと同じ姿勢から始まる
	class ScheduledPlayback {
	public:
		ScheduledPlayback(std::vector<float> durations, std::optional<std::size_t> idle, double idleSeconds, float fadeSeconds, float idleFadeSeconds)
			: m_durations{ std::move(durations) }, m_idle{ idle }, m_idleSeconds{ idleSeconds }, m_fadeSeconds{ fadeSeconds }, m_idleFadeSeconds{ idleFadeSeconds } {}

		void Update(rescue::mascot::Model& model, const Clock& now, double epoch) {
			if (!m_schedule || now.day != m_day) {
				m_schedule.emplace(m_durations, static_cast<std::uint32_t>(now.day), m_idle, m_idleSeconds);
				m_day = now.day;
			}
			const rescue::mascot::ScheduledMotion current = m_schedule->At(now.secondsOfDay);
			if (m_playing == current) return;
			const double startAbsolute = now.absolute - now.secondsOfDay + current.start;
			// 待機に戻るときは、ゆっくりつなぐ
			const float fade = m_idle && current.index == *m_idle ? m_idleFadeSeconds : m_fadeSeconds;
			model.PlayMotion(current.index, fade, static_cast<float>(startAbsolute - epoch));
			m_playing = current;
		}

	private:
		std::vector<float> m_durations;
		std::optional<std::size_t> m_idle;
		double m_idleSeconds;
		float m_fadeSeconds;
		float m_idleFadeSeconds;
		std::optional<rescue::mascot::MotionSchedule> m_schedule;
		std::uint64_t m_day = 0;
		std::optional<rescue::mascot::ScheduledMotion> m_playing;
	};

	// 設定アプリの「再生」の再生のしかた。スケジュールは使わず、決まった順に 1 回だけ再生する:
	//   待機あり: 待機 kLead 秒 → モーション（fadeSeconds でつなぐ）→ 待機（idleFadeSeconds でつなぐ）kTail 秒 → 終わり
	//   待機なし（か、待機モーションそのもの）: モーションを 1 回 → 終わり
	class PreviewSequence {
	public:
		// motion: 再生するモーションの番号。idle: 間に挟む待機モーションの番号（挟まないなら nullopt）
		PreviewSequence(std::size_t motion, std::optional<std::size_t> idle, float fadeSeconds, float idleFadeSeconds)
			: m_motion{ motion }, m_idle{ idle }, m_fadeSeconds{ fadeSeconds }, m_idleFadeSeconds{ idleFadeSeconds } {}

		// seconds の時点まで進める。再生し終わったら true
		bool Update(rescue::mascot::Model& model, float seconds) {
			const float motionSeconds = model.MotionDuration(m_motion);
			if (m_step == Step::NotStarted) {
				m_start = seconds;
				model.PlayMotion(m_idle ? *m_idle : m_motion, 0.0f, seconds);
				m_step = m_idle ? Step::LeadIdle : Step::Motion;
			}
			const float elapsed = seconds - m_start;
			if (m_step == Step::LeadIdle && elapsed >= kLead) {
				model.PlayMotion(m_motion, m_fadeSeconds);
				m_step = Step::Motion;
			}
			if (m_idle && m_step == Step::Motion && elapsed >= kLead + motionSeconds) {
				model.PlayMotion(*m_idle, m_idleFadeSeconds);
				m_step = Step::TailIdle;
			}
			const float end = m_idle ? kLead + motionSeconds + m_idleFadeSeconds + kTail : motionSeconds;
			return elapsed >= end;
		}

	private:
		static constexpr float kLead = 1.0f;
		static constexpr float kTail = 1.0f;
		enum class Step { NotStarted, LeadIdle, Motion, TailIdle };

		std::size_t m_motion;
		std::optional<std::size_t> m_idle;
		float m_fadeSeconds;
		float m_idleFadeSeconds;
		Step m_step = Step::NotStarted;
		float m_start = 0.0f;	// 再生を始めた時刻（seconds の物差し）
	};

	// キャラクターを表示しない場合。キーの案内と、構成変更・ロック状態の通知を受ける非表示ウィンドウだけを使う。
	// 描き続けるものが無いので、MsgWaitForMultipleObjects で「止めて」とメッセージを一緒に待つ（ホットキー監視プロセスの Watcher.cpp と同じ形）
	int RunShortcutsOnly(HANDLE stopEvent) {
		std::vector<rescue::mascot::MonitorArea> monitors = rescue::mascot::EnumerateMonitors();
		std::optional<rescue::mascot::ShortcutCards> cards = MakeCards(monitors);
		if (!cards) {
			Report(L"キーの割り当てがないため、案内を表示せず終了します。");
			return ERROR_NOT_FOUND;
		}
		cards->SetOpacity(1.0f);

		// 知らせを受けるための窓。出さない（WM_DISPLAYCHANGE も WM_WTSSESSION_CHANGE も、見えない窓に届く）
		WindowState state;
		RegisterWindowClass(nullptr);
		const wil::unique_hwnd hwnd{ CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"DisplayRescue shortcuts", WS_POPUP,
			0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), &state) };
		THROW_LAST_ERROR_IF_NULL(hwnd);

		// Winlogon デスクトップでもロックを見張る（UAC の確認画面では出さない。RunMascot と同じ）
		rescue::mascot::SessionLockWatcher lockWatcher{ hwnd.get() };
		state.lockWatcher = &lockWatcher;
		const auto forgetWatcher = wil::scope_exit([&] { state.lockWatcher = nullptr; });
		Report(L"画面切り替えキーの案内だけを表示します(マスコットはオフか、モデルがありません)。");

		auto lastLayoutCheck = std::chrono::steady_clock::now();
		constexpr DWORD kWaitMs = 250;	// 知らせが無くても、この間隔で構成を見比べに戻る
		for (;;) {
			const DWORD count = stopEvent ? 1 : 0;
			if (MsgWaitForMultipleObjects(count, &stopEvent, FALSE, kWaitMs, QS_ALLINPUT) == WAIT_OBJECT_0 && stopEvent) {
				Report(L"止めてと言われたので終わります。");
				return ERROR_SUCCESS;
			}
			MSG message{};
			while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
				if (message.message == WM_QUIT) return ERROR_SUCCESS;
				DispatchMessageW(&message);
			}
			if (const LayoutCheck check = CheckLayout(state, monitors, lastLayoutCheck); check.checked) {
				if (check.changed) {
					cards->Place(monitors);
					Report(std::format(L"画面構成が変わったため、キーの案内を配置し直しました(モニター{}台)", monitors.size()));
				}
				cards->Raise();
			}
			cards->SetVisible(lockWatcher.IsLocked());
		}
	}
}

int rescue::mascot::RunMascot(HANDLE stopEvent, const MascotOptions& options) {
	try {
		SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);	// ぼやけないように（DPI を自分で扱う。Windows に引き伸ばさせない）

		// 置き場所は config/MascotSettings に。サービスと設定アプリも同じ場所を見る
		const std::filesystem::path modelPath = rescue::config::MascotModelPath();
		std::error_code error;
		const bool hasModel = std::filesystem::is_regular_file(modelPath, error);
		// キャラクターを表示しない（オフ、またはモデルがない）場合は、キーの案内だけを表示する。案内もオフなら終了する
		const bool preview = options.previewMotion.has_value();
		if (!options.edit && !preview && (!options.showMascot || !hasModel)) {
			if (options.showShortcuts) return RunShortcutsOnly(stopEvent);
			Report(L"マスコットと画面切り替えキーの案内がどちらもオフのため終了します。");
			return ERROR_SUCCESS;
		}
		if (!hasModel) {
			Report(std::format(L"モデルが無いので終わります: {}", modelPath.wstring()));
			return ERROR_FILE_NOT_FOUND;
		}
		// 試すモーションが待機モーションそのものか。そうなら待機モーションだけを読む（下で読む）
		const std::filesystem::path idlePath = rescue::config::MascotIdleMotionPath();
		const bool previewIsIdle = preview && std::filesystem::equivalent(*options.previewMotion, idlePath, error);
		// フォルダの全部ではなく、試すモーション 1 本だけを読む（読み込みが短くなる）
		const std::vector<std::filesystem::path> motions = !preview ? FindMotions(rescue::config::MascotMotionDirectory())
			: previewIsIdle ? std::vector<std::filesystem::path>{}
			: std::vector<std::filesystem::path>{ *options.previewMotion };

		// 1. 窓。まず画面いっぱいで作る。調整中はこのまま、普段はモデルを読んだ後（大きさが分かってから）
		// キャラを囲む範囲に縮める。キャラの中心（画面の座標）は画面の (positionX, positionY)
		// 画面は、画面の構成が変わったらループの中で読み直す（だから const にしない）
		std::vector<MonitorArea> monitors = EnumerateMonitors();
		RECT screenArea = PrimaryMonitorArea(monitors);
		const RECT placed = screenArea;
		float areaWidth = static_cast<float>(screenArea.right - screenArea.left);
		float areaHeight = static_cast<float>(screenArea.bottom - screenArea.top);

		// 調整中は操作を受ける PlacementEditor を作り、WindowProc から見えるようにする。位置は窓（= 画面）の中の座標。
		// state と editor は窓より先に作る（窓が壊れるときのメッセージでも使われるので、窓より長く生かす）
		std::optional<PlacementEditor> editor;
		if (options.edit) {
			editor.emplace(Placement{ options.positionX * areaWidth, options.positionY * areaHeight, options.scale, options.yaw, options.pitch });
		}
		WindowState state;
		state.editor = editor ? &*editor : nullptr;

		const HINSTANCE instance = GetModuleHandleW(nullptr);
		RegisterWindowClass(options.edit ? LoadCursorW(nullptr, IDC_SIZEALL) : nullptr);	// 「動かせる」形のカーソル

		// 調整中は、クリックを素通しにせず（WS_EX_TRANSPARENT なし）、前面にもなる（WS_EX_NOACTIVATE なし）
		constexpr DWORD kPassThrough = WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
		const DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP | (options.edit ? 0 : kPassThrough);
		const wil::unique_hwnd ownedWindow{ CreateWindowExW(exStyle, kClassName, L"DisplayRescue mascot", WS_POPUP,
			placed.left, placed.top, placed.right - placed.left, placed.bottom - placed.top, nullptr, nullptr, instance, &state) };
		THROW_LAST_ERROR_IF_NULL(ownedWindow);
		const HWND hwnd = ownedWindow.get();
		THROW_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA));

		// 2. 画用紙とモデルとモーション。読み込みに 2 秒ほどかかる
		const auto loadStart = std::chrono::steady_clock::now();
		Canvas canvas{ hwnd, static_cast<UINT>(placed.right - placed.left), static_cast<UINT>(placed.bottom - placed.top) };
		Model model{ canvas.Device(), LoadModelAsset(modelPath) };	// 変換した形式だけを読む（AssetFile.cpp）
		const LoadedMotions loaded = AddMotions(model, motions, idlePath);
		Report(std::format(L"モデルを読みました({} ms、モーション{}本、待機{}、{}): {}",
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - loadStart).count(), loaded.durations.size(),
			loaded.idleIndex ? std::format(L"あり({}秒ごと)", options.motionIntervalSeconds) : std::wstring{ L"なし" }, canvas.Adapter(), model.Info()));

		// どう再生するか。試すときは決まった順に 1 回（読めなかったら終わる。設定アプリが終了コードを見て知らせる）、普段はスケジュールどおり
		std::optional<PreviewSequence> previewSequence;
		std::optional<ScheduledPlayback> playback;
		if (preview) {
			const std::optional<std::size_t> previewIndex = previewIsIdle ? loaded.idleIndex
				: loaded.otherCount > 0 ? std::optional<std::size_t>{ 0 } : std::nullopt;
			if (!previewIndex) {
				Report(std::format(L"試すモーションを読めませんでした: {}", options.previewMotion->wstring()));
				return ERROR_FILE_NOT_FOUND;
			}
			// 待機を挟むのは、待機モーションがあって、試すのが待機モーションそのものでないとき
			previewSequence.emplace(*previewIndex, previewIsIdle ? std::nullopt : loaded.idleIndex, options.fadeSeconds, options.idleFadeSeconds);
		}
		else if (!loaded.durations.empty()) {
			playback.emplace(loaded.durations, loaded.idleIndex, options.motionIntervalSeconds, options.fadeSeconds, options.idleFadeSeconds);
		}

		// 今の置き方（キャラの中心は画面の座標）。調整中は毎フレーム PlacementEditor から取る
		float centerX = screenArea.left + options.positionX * areaWidth;
		float centerY = screenArea.top + options.positionY * areaHeight;
		float scale = options.scale;
		float yaw = options.yaw;
		float pitch = options.pitch;
		// 普段は、キャラを囲む範囲に窓と画用紙を縮める。調整中は画面いっぱい。
		// 画面の構成が変わったときも、ここで配置し直す（だからラムダにした）
		RECT window = placed;
		const auto placeWindow = [&] {
			window = options.edit ? screenArea : MascotRect(screenArea, centerX, centerY, PixelsPerUnit(screenArea, scale), model.Radius());
			SetWindowPos(hwnd, nullptr, window.left, window.top, window.right - window.left, window.bottom - window.top, SWP_NOZORDER | SWP_NOACTIVATE);
			canvas.Resize(static_cast<UINT>(window.right - window.left), static_cast<UINT>(window.bottom - window.top));
		};
		placeWindow();
		const XMFLOAT3 cameraPosition{ 0, 0, -kCameraDistance };
		const XMMATRIX view = XMMatrixLookAtLH(XMLoadFloat3(&cameraPosition), XMVectorZero(), XMVectorSet(0, 1, 0, 0));
		// 調整中は背景をうっすら暗くして、調整中だと分かるようにする（色は透明度を掛けた値。PREMULTIPLIED）
		const std::array<float, 4> clear = options.edit ? std::array<float, 4>{ 0.02f, 0.02f, 0.04f, 0.3f } : std::array<float, 4>{ 0, 0, 0, 0 };

		THROW_LAST_ERROR_IF(SetTimer(hwnd, kTimerRaise, kRaiseIntervalMs, nullptr) == 0);

		// いつも見せるか（調整中、または Default デスクトップでデスクトップにも出す設定）。それ以外はロック中だけ見せる。
		// 窓はまだ出していない。出すか隠すかはループの頭で決める。
		// Winlogon デスクトップは、ロック画面・ログイン画面のほかに UAC の確認画面でも前に出る。
		// UAC の確認画面には出さないよう、Winlogon でもロックを見張り、ロック中だけ見せる
		// （ユーザー不在のログイン画面も、セッションの状態はロック中になる）
		const bool onWinlogon = IsOnWinlogonDesktop();
		// 試しはデスクトップでいつも見せる（ロックは見張らない）
		const bool alwaysShown = options.edit || preview || (options.showOnDesktop && !onWinlogon);
		std::optional<SessionLockWatcher> lockWatcher;
		if (!options.edit && !preview) {
			lockWatcher.emplace(hwnd);
			state.lockWatcher = &*lockWatcher;
		}

		// モニタごとに、今のキーの割り当てを出す（調整中は出さない）。割り当てが 1 つも無ければ作らない。
		// 割り当てが変わるとサービスがマスコットごと再起動する（130）ので、読むのは起動時だけでよい
		std::optional<ShortcutCards> cards;
		if (!options.edit && !preview && options.showShortcuts) cards = MakeCards(monitors);
		auto lastLayoutCheck = std::chrono::steady_clock::now();
		const auto forgetWatcher = wil::scope_exit([&] { state.lockWatcher = nullptr; });	// 抜けるとき（例外でも）、見張りが壊れる前に指す先を消す
		if (options.edit) {
			ShowWindow(hwnd, SW_SHOW);
			SetForegroundWindow(hwnd);	// キーを受けるため。設定アプリ（前面）から起動されたので前面にできる
		}

		// 3. ループ。止めてと言われるまで回る。
		// 時計はプロセスの起動時刻ではなく、OS の時計（UTC）。2 つのマスコットが同じ時刻を見る。
		// Model に渡す seconds は float なので、桁が大きくなりすぎないよう「このループを始めた時刻（epoch）からの秒」にする
		const double epoch = ClockNow().absolute;
		// 見えていないか（前のフレームの Present が S_OK でなかったか）と、見え始めた時刻（seconds の物差し）
		bool hidden = true;
		float visibleSince = 0.0f;
		MSG message{};
		for (;;) {
			while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
				if (message.message == WM_QUIT) return editor ? ERROR_CANCELLED : ERROR_SUCCESS;
				DispatchMessageW(&message);
			}
			// サービスの「止めて」を覗く。待つのではなく覗くだけ（待ち時間 0）。立っていれば ERROR_SUCCESS で終わる。
			// 手で起動したとき（ハンドルなし）は nullptr で、閉じるまで回る
			if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
				Report(L"止めてと言われたので終わります。");
				return ERROR_SUCCESS;
			}

			// 知らせが来たか、前に調べてから 1 秒たったら、モニタの一覧を調べ直す。変わっていたら配置し直す。
			// 隠しているときも調べる（ロックを解除している間に構成が変わることもあるので）
			if (const LayoutCheck check = CheckLayout(state, monitors, lastLayoutCheck); check.checked) {
				if (check.changed) {
					screenArea = PrimaryMonitorArea(monitors);
					areaWidth = static_cast<float>(screenArea.right - screenArea.left);
					areaHeight = static_cast<float>(screenArea.bottom - screenArea.top);
					// 普段は、設定の割合から中心を出し直す（調整中は PlacementEditor が窓の中の座標で持っているので、そのまま）
					if (!editor) {
						centerX = screenArea.left + options.positionX * areaWidth;
						centerY = screenArea.top + options.positionY * areaHeight;
					}
					placeWindow();
					if (cards) cards->Place(monitors);
					Report(std::format(L"画面の構成が変わったので配置し直しました(モニター{}台、メインモニター {},{} {}x{})",
						monitors.size(), screenArea.left, screenArea.top, screenArea.right - screenArea.left, screenArea.bottom - screenArea.top));
				}
				if (cards) cards->Raise();	// キーの案内も最前面に戻す（マスコットのウィンドウは kTimerRaise で戻す）
			}

			// 見せないとき（ロックしていない普段のデスクトップ）は、窓を隠して描かずに休む。
			// hidden を true にしておくので、また見せたとき（ロックしたとき）は起動したときと同じくふわっと出る
			const bool locked = lockWatcher && lockWatcher->IsLocked();
			const bool shown = alwaysShown || locked;
			if (!shown) {
				if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
				if (cards) cards->SetVisible(false);
				hidden = true;
				Sleep(100);
				continue;
			}
			if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
			// キーの案内はロック中だけ表示する（Default ではロック画像、Winlogon ではパスワード欄・ログイン画面）
			if (cards) cards->SetVisible(locked);

			// 調整中: 置き方を PlacementEditor から取る（位置は窓の中の座標なので、画面の座標に直す）
			if (editor) {
				const Placement& p = editor->Current();
				centerX = window.left + p.centerX;
				centerY = window.top + p.centerY;
				scale = p.scale;
				yaw = p.yaw;
				pitch = p.pitch;
			}

			// 調整が終わった。Enter なら、置き方を設定に書いて終わる（ほかの設定はそのまま）
			if (editor && editor->IsFinished()) {
				if (!editor->IsAccepted()) return ERROR_CANCELLED;
				rescue::config::MascotSettings settings = rescue::config::LoadMascotSettings();
				settings.positionX = static_cast<int>(std::lround((centerX - screenArea.left) / areaWidth * 1000));
				settings.positionY = static_cast<int>(std::lround((centerY - screenArea.top) / areaHeight * 1000));
				settings.scalePercent = static_cast<int>(std::lround(scale * 100));
				settings.yawDegrees = static_cast<int>(std::lround(XMConvertToDegrees(yaw)));
				settings.pitchDegrees = static_cast<int>(std::lround(XMConvertToDegrees(pitch)));
				rescue::config::SaveMascotSettings(settings);
				return ERROR_SUCCESS;
			}

			const Clock now = ClockNow();
			const float seconds = static_cast<float>(now.absolute - epoch);
			// まばたき。その日の何秒目かで決めるので、2 つのマスコットで同じ瞬間にまばたきする
			const float blinkTime = static_cast<float>(std::fmod(now.secondsOfDay, static_cast<double>(options.blinkInterval)));
			const float half = kBlinkTime / 2;
			model.SetExpression(L"blink", blinkTime < half ? blinkTime / half : blinkTime < kBlinkTime ? 1.0f - (blinkTime - half) / half : 0.0f);

			// 再生するモーションを決める
			if (previewSequence) {
				if (previewSequence->Update(model, seconds)) return ERROR_SUCCESS;
			}
			else if (playback) {
				playback->Update(model, now, epoch);
			}

			// 見えていない間は（ほぼ）透明にしておき、見え始めたら appearSeconds かけて出てくる。
			// 完全な 0 にしないのは、透明すぎて「見えていない」とみなされ続けるのを念のため避けるため（1/255 なら目には見えない）
			const float opacity = hidden ? kHiddenOpacity : std::clamp((seconds - visibleSince) / options.appearSeconds, kHiddenOpacity, 1.0f);
			canvas.SetOpacity(opacity);
			// キーの案内もマスコットと同時に表示する。別のデスクトップが前面にありマスコットが見えない場合は、案内も表示されない
			if (cards) cards->SetOpacity(opacity);
			// 向きはモデルを回す（縦の軸まわり → 横の軸まわり）。大きさと位置は射影で
			const XMMATRIX world = XMMatrixRotationY(yaw) * XMMatrixRotationX(pitch);
			const XMMATRIX viewProjection = view * ProjectionFor(window, centerX, centerY, PixelsPerUnit(screenArea, scale));
			canvas.Begin(clear);
			model.Draw(canvas.Context(), world, viewProjection, cameraPosition, seconds);
			// 見えていない側は S_OK 以外（DXGI_STATUS_OCCLUDED。成功のうちの 1 つ）がすぐ返る。
			// 失敗（GPU が外れた DXGI_ERROR_DEVICE_REMOVED など）は作り直せないので、例外で終わる。サービスが 5 秒後に再起動する
			const HRESULT presented = canvas.End();
			THROW_IF_FAILED(presented);
			const bool nowHidden = presented != S_OK;
			// 「見えていなかった → 見えた」に変わったフレームで、見え始めた時刻（visibleSince）を今（seconds）にする
			if (hidden && !nowHidden) visibleSince = seconds;
			hidden = nowHidden;
			if (hidden) Sleep(50);	// CPU を使い切らないよう少し休む
		}
	}
	catch (...) {
		const HRESULT hr = wil::ResultFromCaughtException();
		Report(std::format(L"失敗しました: {}", rescue::DescribeHresult(hr)));
		return rescue::ExitCodeFromHresult(hr);
	}
}
