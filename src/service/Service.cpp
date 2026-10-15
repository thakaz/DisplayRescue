#include "service/Service.h"

#include <windows.h>
#include <wtsapi32.h>       // WTS_SESSION_LOCK などセッション変更の種類、WTSSESSION_NOTIFICATION
#include <wil/resource.h>   // wil::unique_event
#include <wil/result.h>     // THROW_LAST_ERROR_IF_NULL など

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "common/Common.h"
#include "common/Log.h"
#include "config/MascotSettings.h"
#include "service/ResidentProcess.h"
#include "service/SessionLauncher.h"

// Windows サービス本体。SCM との通信、セッション変更の振り分け、子プロセスの終了と再起動予定を処理する。
// 常駐させる子プロセス（ホットキー監視プロセス・マスコット 2 つ）の起動・停止・再起動は ResidentProcess が管理する
namespace {
	using namespace rescue::service;

	// ServiceMain /ControlHandlerはSCMからCの関数として呼ばれ、自分のオブジェクトは渡してもらえない（ServiceMainにはcontext引数も無い）。
	// そのためサービスの状態はここに一つだけ置く。
	struct ServiceContext {
		SERVICE_STATUS_HANDLE statusHandle = nullptr; //CloseHandleは不要。SCMが管理する。
		SERVICE_STATUS status{};
		DWORD checkPoint = 1;

		wil::unique_event stopEvent;	//停止要求(手動リセット)
		wil::unique_event workEvent;	// 再起動要求（自動リセット）

		// セッション変更通知によるホットキー監視プロセスの再起動要求。ログ用に最後の通知種別とセッションも記録する
		std::atomic<bool>  placeRequested{ false };
		std::atomic<DWORD> lastSessionEvent{ 0 };
		std::atomic<DWORD> lastSessionId{ 0 };

		// bind / unbind からの設定再読み込み要求（ホットキー監視プロセスは起動時にしか読み込まないため、再起動して反映する）
		std::atomic<bool>  reloadRequested{ false };
	};

	ServiceContext g_service;

	// SCMに現在の状態を知らせる。
	// *_PENDINGの間はcheckPointを増やし続けて固まってないことをSCMに示す。
	void ReportStatus(DWORD state, DWORD win32ExitCode = NO_ERROR, DWORD waitHintMs = 0) {
		SERVICE_STATUS& s = g_service.status;
		s.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
		s.dwCurrentState = state;
		s.dwWin32ExitCode = win32ExitCode;
		s.dwWaitHint = waitHintMs;
		// SESSIONCHANGE: ロック・解除・サインインなどの通知を受け取る（RegisterServiceCtrlHandlerExW で登録したハンドラにだけ届く）
		s.dwControlsAccepted = (state == SERVICE_RUNNING) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE) : 0;
		s.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : g_service.checkPoint++;
		SetServiceStatus(g_service.statusHandle, &s);
	}

	// ホットキー監視プロセスを再起動するセッション変更の名前。それ以外は空
	//   CONSOLE_CONNECT: 物理コンソールにセッションがつながった（サインアウト後は新しい番号のセッションになる）
	//   LOGON / LOCK / UNLOCK: 入力デスクトップが Default と Winlogon の間で入れ替わる
	// サインアウト（LOGOFF）ではユーザー側のホットキー監視プロセスが終了するため、子プロセスの終了として検出する
	std::wstring_view PlacementEventName(DWORD eventType) {
		switch (eventType) {
		case WTS_CONSOLE_CONNECT:	return L"コンソール接続";
		case WTS_SESSION_LOGON:		return L"サインイン";
		case WTS_SESSION_LOCK:		return L"ロック";
		case WTS_SESSION_UNLOCK:	return L"ロック解除";
		default:					return {};
		}
	}

	// コントロール ハンドラ。SCM のディスパッチャ スレッド（= wmain のスレッド）で呼ばれる。
	// ここで重い処理をすると他の制御が詰まるので、要求を記録してイベントを立てるだけにする。
	DWORD WINAPI ControlHandler(DWORD control, DWORD eventType, LPVOID eventData, LPVOID /*context*/)
	{
		switch (control)
		{
		case SERVICE_CONTROL_STOP:
		case SERVICE_CONTROL_SHUTDOWN:
			g_service.stopEvent.SetEvent();
			return NO_ERROR;

		case SERVICE_CONTROL_INTERROGATE:
			return NO_ERROR;

			// eventType が通知の種類（WTS_SESSION_LOCK など）、eventData がどのセッションか（WTSSESSION_NOTIFICATION）
		case SERVICE_CONTROL_SESSIONCHANGE:
			if (!PlacementEventName(eventType).empty()) {
				const DWORD sessionId = static_cast<const WTSSESSION_NOTIFICATION*>(eventData)->dwSessionId;
				// サインイン・ロック・ロック解除は、物理コンソールのセッションのものだけを見る。
				// リモートデスクトップのセッションをロックしても、物理画面の入力デスクトップは変わらないため再起動しない
				// （再起動中はキーが一時的に無効になる）。コンソール接続ではセッションIDが変わるため、常に処理する
				if (eventType != WTS_CONSOLE_CONNECT && sessionId != WTSGetActiveConsoleSessionId()) {
					return NO_ERROR;
				}
				g_service.lastSessionEvent = eventType;
				g_service.lastSessionId = sessionId;
				g_service.placeRequested = true;
				g_service.workEvent.SetEvent();
			}
			return NO_ERROR;

		case kControlReload:
			g_service.reloadRequested = true;
			g_service.workEvent.SetEvent();
			return NO_ERROR;

		default:
			return ERROR_CALL_NOT_IMPLEMENTED;
		}
	}

	// マスコットを置くか。キャラクタは、設定（HKLM\SOFTWARE\DisplayRescue\Mascot の Enabled）がオンで、モデルのファイルがあるとき。
	// 画面切り替えキーの案内（ShowShortcuts）がオンなら、キャラクターがオフでも DisplayRescueMascot.exe を起動する。
	// 起動のたびに確認するため、設定アプリで保存すると制御コード 130 による再起動後すぐに反映される。
	//
	// 置くのはロック中（ログイン画面も）だけ。1 つで 200 MB ほど使うので、普段の作業中は 2 つとも止めておく
	// （ロックしてから読み込むので、出てくるのは 2 秒ほど遅れる）。
	//   「デスクトップにも出す」でキャラクタを出すときは、ロック画像用（Default デスクトップ）だけ普段も置く。
	//   「常駐させる」（KeepResident）なら、2 つともいつも置く（ロックしてすぐ出る）
	struct WantedMascots {
		bool winlogon = false;	// パスワード欄用
		bool lockImage = false;	// ロック画像用
	};
	WantedMascots WantedMascotsFor(bool locked) {
		try {
			const auto settings = rescue::config::LoadMascotSettings();
			std::error_code error;
			const bool character = settings.enabled && std::filesystem::is_regular_file(rescue::config::MascotModelPath(), error);
			const bool anything = character || settings.showShortcuts;
			// 常駐させる設定（KeepResident）なら、ロックしていなくても置いておく（ロックしてすぐ出る）
			const bool now = locked || settings.keepResident;
			return { anything && now, anything && (now || (character && settings.showOnDesktop)) };
		}
		catch (...) {
			return {};
		}
	}

	// 今ロック中か。ロック・解除の知らせならそれを信じ、それ以外は調べる。
	// 調べられなければロック中とみなす（置いておけば、少なくとも出るべきときに出ないことはない）
	bool LockedNow(std::optional<DWORD> sessionEvent) {
		if (sessionEvent == static_cast<DWORD>(WTS_SESSION_LOCK)) return true;
		if (sessionEvent == static_cast<DWORD>(WTS_SESSION_UNLOCK)) return false;
		try {
			return IsConsoleSessionLocked();
		}
		catch (...) {
			return true;
		}
	}

	// 最も近い再起動予定までのミリ秒。予定がなければ INFINITE、予定時刻を過ぎていれば 0
	DWORD MillisecondsUntilRetry(std::span<ResidentProcess* const> residents) {
		std::optional<std::chrono::steady_clock::time_point> earliest;
		for (const ResidentProcess* resident : residents) {
			if (const auto at = resident->RetryAt(); at && (!earliest || *at < *earliest)) earliest = at;
		}
		if (!earliest) return INFINITE;
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*earliest - std::chrono::steady_clock::now());
		return left.count() > 0 ? static_cast<DWORD>(left.count()) : 0;
	}

	// マスコットを置く・止める。置かないもの（WantedMascotsFor）は止める。
	// restart が true なら動作中でも再起動する（コンソール接続によるセッションID変更時・設定の再読み込み時）。false なら停止中のものだけ起動する
	void PlaceMascots(ResidentProcess& winlogon, ResidentProcess& lockImage, bool locked, bool restart) {
		const WantedMascots wanted = WantedMascotsFor(locked);
		const auto place = [restart](ResidentProcess& mascot, bool want) {
			mascot.ResetRetryDelay();
			if (!want) mascot.Stop();
			else if (restart || !mascot.IsRunning()) mascot.Start();
		};
		place(winlogon, wanted.winlogon);
		place(lockImage, wanted.lockImage);
	}

	// サービス本体(SCMが別スレッドで呼ぶ)
	void WINAPI ServiceMain(DWORD /*argc*/, LPWSTR* /*argv*/) {
		// 置きっぱなしにする子。ServiceMain の中の変数なので、抜けるときに後片付けされる。
		// ResidentProcess はイベントを作るときに例外を投げうるので、optional で「後から try の中で作る」
		std::optional<ResidentProcess> watcher;
		std::optional<ResidentProcess> mascotWinlogon;
		std::optional<ResidentProcess> mascotLockImage;
		try {
			// ハンドラを登録する前にイベントを作る(登録直後から制御が届く可能性があるため)
			// unique_event::createは失敗すると例外を投げる。
			g_service.stopEvent.create(wil::EventOptions::ManualReset);
			g_service.workEvent.create(wil::EventOptions::None);

			// 名前・サブコマンド・起動のしかた（Launcher は std::function なので、ラムダで渡す）
			//   ホットキー監視プロセス: 入力デスクトップ（ロック中は SYSTEM 権限で Winlogon、それ以外はユーザー権限で Default）。入力デスクトップの変更に合わせて再起動する
			//   マスコット(パスワード欄): いつも SYSTEM で Winlogon。ユーザーがいてもいなくても置く
			//   マスコット(ロック画像): いつもユーザー＋UIAccess で Default。ロック画像（LockApp）の上に出る。ユーザーがいるときだけ
			//   マスコットは別の exe（DisplayRescueMascot.exe）なので、サブコマンドは空にし、同じフォルダの kMascotExeName を起動する
			//   （どちらのマスコットもロック中だけ。PlaceMascots）
			watcher.emplace(L"ホットキー監視プロセス", L"watch", [](std::wstring_view arguments, HANDLE inheritHandle) {
				return StartInInputDesktop(arguments, inheritHandle);
			});
			mascotWinlogon.emplace(L"マスコット(パスワード欄)", L"", [](std::wstring_view arguments, HANDLE inheritHandle) {
				return StartInWinlogonDesktop(arguments, inheritHandle, kMascotExeName);
			});
			mascotLockImage.emplace(L"マスコット(ロック画像)", L"", [](std::wstring_view arguments, HANDLE inheritHandle) {
				return StartInUserDesktopWithUiAccess(arguments, inheritHandle, kMascotExeName);
			});

			g_service.statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ControlHandler, nullptr);
			// nullptrならGetLastErrorの値で例外を投げる。
			THROW_LAST_ERROR_IF_NULL(g_service.statusHandle);
		}
		catch (...) {
			//失敗の詳細はWILのログがrescue.logに書いている。
			return;
		}

		// &*watcher: optional の中身（*watcher）のアドレス
		ResidentProcess* const residents[] = { &*watcher, &*mascotWinlogon, &*mascotLockImage };

		// 準備に時間のかかるものは無いので、START_PENDING を挟まずすぐ RUNNING にする
		ReportStatus(SERVICE_RUNNING);
		rescue::Log(std::format(L"サービス開始(コンソールセッション{})", WTSGetActiveConsoleSessionId()));

		// 起動時に一度置く（再起動直後のログイン画面でも、何もしなくてもキーが効くように）
		watcher->Start();
		PlaceMascots(*mascotWinlogon, *mascotLockImage, LockedNow(std::nullopt), false);

		// 停止要求・再起動要求・子プロセスの終了・再起動予定時刻を待つ
		// 同時に立っていたらindexの小さいほう(停止)が優先される
		for (;;) {
			// 動いている子のプロセスだけ並べる。owners[i] は handles[2 + i] がどの子のものか
			std::vector<HANDLE> handles{ g_service.stopEvent.get(), g_service.workEvent.get() };
			std::vector<ResidentProcess*> owners;
			for (ResidentProcess* resident : residents) {
				if (resident->IsRunning()) {
					handles.push_back(resident->ProcessHandle());
					owners.push_back(resident);
				}
			}
			const DWORD wait = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, MillisecondsUntilRetry(residents));

			if (wait == WAIT_OBJECT_0 + 1) {
				// セッション変更と再読み込み要求が重なっても、再起動は 1 回だけ行う
				const bool sessionChanged = g_service.placeRequested.exchange(false);
				const bool reload = g_service.reloadRequested.exchange(false);
				std::optional<DWORD> sessionEvent;
				if (sessionChanged) {
					sessionEvent = g_service.lastSessionEvent;
					rescue::Log(std::format(L"セッション変更: {}(セッション{}、コンソールセッション{})",
						PlacementEventName(*sessionEvent), g_service.lastSessionId.load(), WTSGetActiveConsoleSessionId()));
				}
				if (reload) rescue::Log(L"設定の読み直しを要求されました");
				if (sessionChanged || reload) {
					// ホットキー監視プロセスは入力デスクトップの変更に合わせて再起動する。キーを早く有効にするため、マスコットより先に処理する
					watcher->ResetRetryDelay();	// 状況が変わったので、間隔は最初から
					watcher->Start();
					// マスコットはロックしたら置き、解除したら止める。
					// 動作中のプロセスを再起動するのは、コンソール接続（セッションIDの変更）と設定の再読み込みの場合
					// （設定アプリ・bind / unbind から。マスコットは起動時にしか設定を読まないため。読み込みに 2 秒ほどかかる）
					const bool restart = reload || sessionEvent == static_cast<DWORD>(WTS_CONSOLE_CONNECT);	// WTS_* は int のマクロなので DWORD にそろえる（optional との比較で符号の警告が出る）
					PlaceMascots(*mascotWinlogon, *mascotLockImage, LockedNow(sessionEvent), restart);
				}
				continue;
			}
			if (wait >= WAIT_OBJECT_0 + 2 && wait < WAIT_OBJECT_0 + handles.size()) {
				owners[wait - WAIT_OBJECT_0 - 2]->OnExited();
				continue;
			}
			if (wait == WAIT_TIMEOUT) {
				// 再起動予定時刻を過ぎた子プロセスを起動する
				const auto now = std::chrono::steady_clock::now();
				for (ResidentProcess* resident : residents) {
					if (const auto at = resident->RetryAt(); at && *at <= now) resident->Start();
				}
				continue;
			}
			if (wait != WAIT_OBJECT_0) {
				rescue::Log(std::format(L"WaitForMultipleObjectsが失敗しました: {}", GetLastError()));
			}
			break;	//停止要求 or 待機失敗
		}

		// 子は最大 5 秒ずつ待つので、SCM にはそれより長い見込み時間を伝えておく
		ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 20000);
		for (ResidentProcess* resident : residents) resident->Stop();	// サービスがいなくなった後に子だけ残さない
		rescue::Log(L"サービス停止");
		ReportStatus(SERVICE_STOPPED);
	}
}


rescue::service::ServiceState rescue::service::QueryServiceState()
{
	// 状態を見るだけなので、SCM には接続だけ、サービスには SERVICE_QUERY_STATUS だけを求める（一般ユーザーでも通る）
	wil::unique_schandle manager{ OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT) };
	if (!manager) return ServiceState::Unknown;
	wil::unique_schandle service{ OpenServiceW(manager.get(), kServiceName, SERVICE_QUERY_STATUS) };
	if (!service) {
		return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? ServiceState::NotInstalled : ServiceState::Unknown;
	}

	SERVICE_STATUS_PROCESS status{};
	DWORD needed = 0;
	if (!QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) {
		return ServiceState::Unknown;
	}
	switch (status.dwCurrentState) {
	case SERVICE_RUNNING: return ServiceState::Running;
	case SERVICE_STOPPED: return ServiceState::Stopped;
	default:              return ServiceState::Pending;
	}
}

namespace {
	//ユーザー定義のコントロールコードを送る。サービスがない・止まっているときはfalse
	bool SendUserControl(DWORD control) {
		//コントロールを送るだけなので、SCMには接続だけ、サービスにはSERVICE_USER_DEFINED_CONTROLだけを求める
		wil::unique_schandle manager{ OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT) };
		if (!manager) return false;
		wil::unique_schandle service{ OpenServiceW(manager.get(),kServiceName,SERVICE_USER_DEFINED_CONTROL) };
		if (!service) return false;

		SERVICE_STATUS status{};
		return ControlService(service.get(), control, &status) != FALSE;	//止まっているとERROR_SERVICE_NOT_ACTIVEでFALSE
	}
}

bool rescue::service::RequestServiceReload()
{
	return SendUserControl(kControlReload);
}

int rescue::service::RunService()
{
	rescue::RouteWilFailuresToLog();

	// SERVICE_WIN32_OWN_PROCESS（1 プロセス 1 サービス）では名前は使われないが、nullptr は不可
	const SERVICE_TABLE_ENTRYW table[] = {
		{ const_cast<LPWSTR>(kServiceName), ServiceMain },
		{ nullptr, nullptr },
	};

	// SCM に接続し、ServiceMain を別スレッドで呼んでもらう。
	// このスレッドは以後、コントロール ハンドラを呼ぶための「ディスパッチャ」になる。
	if (!StartServiceCtrlDispatcherW(table))
	{
		const DWORD err = GetLastError();
		// コンソールから手で実行すると ERROR_FAILED_SERVICE_CONTROLLER_CONNECT (1063)
		std::wcout << std::format(L"サービスとして起動できません(SCMから起動されたときだけ使うコマンドです): {} ({})\n",
			rescue::DescribeError(static_cast<LONG>(err)), err);
		return static_cast<int>(err);
	}
	return 0;
}
