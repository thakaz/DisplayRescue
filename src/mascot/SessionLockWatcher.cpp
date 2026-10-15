#include "mascot/SessionLockWatcher.h"

#include <WtsApi32.h>
#include <wil/result.h>

#include "common/Common.h"

#pragma comment(lib, "wtsapi32.lib")

namespace rescue::mascot {
	// 1. 通知を頼み、今の状態を調べる
	SessionLockWatcher::SessionLockWatcher(HWND hwnd) : m_hwnd{ hwnd } {
		// NOTIFY_FOR_THIS_SESSION: 自分のセッションの変化だけ
		THROW_IF_WIN32_BOOL_FALSE(WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION));

		// 今ロック中か。調べられなければ通知を取り消してから例外を投げ直す（デストラクタは呼ばれないため）
		try {
			m_locked = rescue::IsSessionLocked(WTS_CURRENT_SESSION);
		}
		catch (...) {
			WTSUnRegisterSessionNotification(hwnd);
			throw;
		}
	}

	// 2. 頼んだ通知を取り消す
	SessionLockWatcher::~SessionLockWatcher() {
		WTSUnRegisterSessionNotification(m_hwnd);
	}

	// 3. ロックと解除（とサインイン）だけ見る。ほか（リモートの接続など）は今の状態を変えない
	void SessionLockWatcher::OnSessionChange(WPARAM event) noexcept {
		switch (event) {
		case WTS_SESSION_LOCK:
			m_locked = true;
			break;
		case WTS_SESSION_UNLOCK:
		// ログイン画面（ユーザー不在、状態はロック中）からサインインしたときは、
		// 解除ではなくサインインの知らせが来る。来ないままだとロック中のままになり、Winlogon 側が UAC の確認画面で出てしまう
		case WTS_SESSION_LOGON:
			m_locked = false;
			break;
		default:
			break;
		}
	}
}
