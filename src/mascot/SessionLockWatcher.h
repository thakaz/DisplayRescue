#pragma once

#include <windows.h>

namespace rescue::mascot {
	// 自分のセッションがロック中かを見張る。
	// 作るときに今の状態を調べ（WTSQuerySessionInformationW の WTSSessionInfoEx。SessionLauncher.cpp の IsSessionLocked と同じ）、
	// あとは窓に届くセッション変更の通知（WM_WTSSESSION_CHANGE）で追いかける。通知を頼むのは WTSRegisterSessionNotification、
	// やめるのは WTSUnRegisterSessionNotification（デストラクタで。頼んだまま窓が消えないように）。
	// 使うのは Mascot.cpp: ロック画像用のマスコット（Default デスクトップ）を、ロック中だけ見せ、普段のデスクトップでは隠す
	class SessionLockWatcher {
	public:
		// hwnd: 通知を受け取る窓（このプロセスの窓。WindowProc で WM_WTSSESSION_CHANGE を受けて OnSessionChange を呼ぶ）。失敗は例外
		explicit SessionLockWatcher(HWND hwnd);
		~SessionLockWatcher();

		// 通知の頼みを 1 つ持つので、コピーはできない（2 つのオブジェクトが同じ頼みを取り消さないように）
		SessionLockWatcher(const SessionLockWatcher&) = delete;
		SessionLockWatcher& operator=(const SessionLockWatcher&) = delete;

		// WM_WTSSESSION_CHANGE の wParam（WTS_SESSION_LOCK / WTS_SESSION_UNLOCK など）を渡す。ロックと解除だけを見て、ほかは無視する
		void OnSessionChange(WPARAM event) noexcept;

		bool IsLocked() const noexcept { return m_locked; }

	private:
		HWND m_hwnd;
		bool m_locked = false;
	};
}
