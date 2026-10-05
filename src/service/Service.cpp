#include "service/Service.h"

#include <windows.h>
#include <wtsapi32.h>       // WTS_SESSION_LOCK などセッション変更の種類、WTSSESSION_NOTIFICATION
#include <wil/resource.h>   // wil::unique_event
#include <wil/result.h>     // THROW_LAST_ERROR_IF_NULL など

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <iostream>
#include <optional>
#include <string_view>

#include "common/Common.h"
#include "common/Log.h"
#include "service/SessionLauncher.h"

namespace {
	using namespace rescue::service;

	// 待ち受け役に「止めて」を送ってから終わるまで待つ上限。過ぎたら強制終了する
	constexpr std::chrono::seconds kWatcherStopTimeout{ 5 };

	// 待ち受け役を置けなかったとき・待ち受け役が自分で終わったときに置き直すまでの間隔。
	// 失敗が続くたびに倍にし、上限で止める（設定ミスなどで毎回すぐ終わるときにログを埋めないため）
	constexpr std::chrono::seconds kRetryFirst{ 5 };
	constexpr std::chrono::seconds kRetryMax{ 300 };

	// ServiceMain /ControlHandlerはSCMからCの関数として呼ばれ、自分のオブジェクトは渡してもらえない（ServiceMainにはcontext引数も無い）。
	// そのためサービスの状態はここに一つだけ置く。
	struct ServiceContext {
		SERVICE_STATUS_HANDLE statusHandle = nullptr; //CloseHandleは不要。SCMが管理する。
		SERVICE_STATUS status{};
		DWORD checkPoint = 1;

		wil::unique_event stopEvent;	//停止要求(手動リセット)
		wil::unique_event workEvent;	//置き直し要求(自動リセット)

		// セッション変更の通知で待ち受け役を置き直す要求。ログ用に最後の通知の種類とセッションも残す
		std::atomic<bool>  placeRequested{ false };
		std::atomic<DWORD> lastSessionEvent{ 0 };
		std::atomic<DWORD> lastSessionId{ 0 };

		// bind / unbind からの「割り当てを読み直して」（待ち受け役は起動時にしか読まないので、置き直して読ませる）
		std::atomic<bool>  reloadRequested{ false };

		//ここから下は ServiceMain のスレッドだけが触る。
		wil::unique_process_information watcher;	//うごいていなければ hProcessがnullptr
		wil::unique_event watcherStopEvent;			//待ち受け役への「止めて」(手動リセット)
		std::chrono::seconds retryDelay = kRetryFirst;	//次に置き直すまでの間隔
		std::optional<std::chrono::steady_clock::time_point> retryAt;	//置き直す予定の時刻。予定がなければ nullopt
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

	// 待ち受け役を置き直すきっかけになるセッション変更の名前。それ以外は空
	//   CONSOLE_CONNECT: 物理コンソールにセッションがつながった（サインアウト後は新しい番号のセッションになる）
	//   LOGON / LOCK / UNLOCK: 入力デスクトップが Default と Winlogon の間で入れ替わる
	// サインアウト（LOGOFF）ではユーザーの待ち受け役が消されるので、「自分で終わった」として拾う
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
				// リモートデスクトップのセッションをロックしても、物理画面の入力デスクトップは変わらないので置き直さない
				// (置き直すあいだはキーが一瞬効かなくなる)。コンソール接続は、セッションの番号が変わるので常に見る
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

	//待ち受け役を止める。動いていなければ何もしない。
	//まず「止めて」のイベントで頼み、kWatcherStopTimeout 以内に終わらなければ強制終了する
	void StopWatcher() {
		if (!g_service.watcher.hProcess) return; //動いてないとき

		g_service.watcherStopEvent.SetEvent();
		DWORD wait = WaitForSingleObject(g_service.watcher.hProcess, static_cast<DWORD>(std::chrono::milliseconds(kWatcherStopTimeout).count()));
		if (wait == WAIT_TIMEOUT) {
			rescue::Log(L"待ち受け役が止まらないため強制終了します");
			TerminateProcess(g_service.watcher.hProcess, ERROR_TIMEOUT);
			wait = WaitForSingleObject(g_service.watcher.hProcess, INFINITE);
		}

		DWORD exitCode = 0;
		GetExitCodeProcess(g_service.watcher.hProcess, &exitCode);
		rescue::Log(std::format(L"待ち受け役が終了: {} ({})", rescue::DescribeError(static_cast<LONG>(exitCode)), exitCode));
		g_service.watcher.reset();
	}

	//待ち受け役を入力デスクトップに起動する。動いていれば止めてから起動し直す（同じキーは 1 つしか登録できないため、必ず先に止める）。
	//起動できたら true
	bool StartWatcher() {
		StopWatcher();

		try {
			g_service.watcherStopEvent.ResetEvent();

			//子に渡すのは待つだけできる継承可能なコピー(SYNCHRONIZE)
			//待ち受け役はユーザーの身分で動くこともあるので、イベントを立てたり戻したりはさせない。
			wil::unique_handle childStopEvent;
			THROW_IF_WIN32_BOOL_FALSE(
				DuplicateHandle(GetCurrentProcess(), g_service.watcherStopEvent.get(), GetCurrentProcess(), childStopEvent.put(), SYNCHRONIZE, TRUE, 0));

			//継承したハンドルは子でも同じ値になるので、その値をコマンドラインで教える。
			g_service.watcher = StartInInputDesktop(std::format(L"watch {}", HandleToULong(childStopEvent.get())), childStopEvent.get());
			rescue::Log(std::format(L"待ち受け役を起動(プロセス{}、コンソールセッション{})", g_service.watcher.dwProcessId, WTSGetActiveConsoleSessionId()));
			return true;
			//childStopEventはここで閉じる。子は継承した自分のコピーを持っている
		}
		catch (...) {
			rescue::Log(std::format(L"待ち受け役を起動できませんでした： {}", rescue::DescribeHresult(wil::ResultFromCaughtException())));
			return false;
		}
	}

	//retryDelay 後に置き直す予定を入れ、次の間隔を倍にする（上限 kRetryMax）
	void ScheduleRetry() {
		g_service.retryAt = std::chrono::steady_clock::now() + g_service.retryDelay;
		rescue::Log(std::format(L"{}秒後に待ち受け役を置き直します", g_service.retryDelay.count()));
		//(std::min) と括弧で囲むのは、windows.h が min をマクロとして定義していて std::min( がマクロに化けるのを防ぐため
		g_service.retryDelay = (std::min)(g_service.retryDelay * 2, kRetryMax);
	}

	//待ち受け役を置く。置けたら予定を消し、置けなければ置き直す予定を入れる
	void PlaceWatcher() {
		g_service.retryAt.reset();
		if (!StartWatcher()) {
			ScheduleRetry();
		}
	}

	//待ち受け役が自分で終わった（落ちた・サインアウトで消された・登録できるキーが無かった）。終了コードを残して置き直す予定を入れる
	void OnWatcherExited() {
		DWORD exitCode = 0;
		GetExitCodeProcess(g_service.watcher.hProcess, &exitCode);
		rescue::Log(std::format(L"待ち受け役が自分で終了しました: {} ({})", rescue::DescribeError(static_cast<LONG>(exitCode)), exitCode));
		g_service.watcher.reset();
		ScheduleRetry();
	}

	//置き直す予定の時刻までのミリ秒。予定がなければ INFINITE、過ぎていれば 0
	DWORD MillisecondsUntilRetry() {
		if (!g_service.retryAt) return INFINITE;
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*g_service.retryAt - std::chrono::steady_clock::now());
		return left.count() > 0 ? static_cast<DWORD>(left.count()) : 0;
	}

	// サービス本体(SCMが別スレッドで呼ぶ)
	void WINAPI ServiceMain(DWORD /*argc*/, LPWSTR* /*argv*/) {
		try {
			// ハンドラを登録する前にイベントを作る(登録直後から制御が届く可能性があるため)
			// unique_event::createは失敗すると例外を投げる。
			g_service.stopEvent.create(wil::EventOptions::ManualReset);
			g_service.workEvent.create(wil::EventOptions::None);
			g_service.watcherStopEvent.create(wil::EventOptions::ManualReset);

			g_service.statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ControlHandler, nullptr);
			// nullptrならGetLastErrorの値で例外を投げる。
			THROW_LAST_ERROR_IF_NULL(g_service.statusHandle);
		}
		catch (...) {
			//失敗の詳細はWILのログがrescue.logに書いている。
			return;
		}

		// 準備に時間のかかるものは無いので、START_PENDING を挟まずすぐ RUNNING にする
		ReportStatus(SERVICE_RUNNING);
		rescue::Log(std::format(L"サービス開始(コンソールセッション{})", WTSGetActiveConsoleSessionId()));

		// 起動時に一度置く（再起動直後のログイン画面でも、何もしなくてもキーが効くように）
		PlaceWatcher();

		// 停止要求・置き直し要求・待ち受け役の終了・置き直しの予定時刻を待つ
		// 同時に立っていたらindexの小さいほう(停止)が優先される
		for (;;) {
			// 待ち受け役が動いているときだけ、そのプロセスの終了も待つ（動いていないときの hProcess は nullptr で、待てない）
			const HANDLE handles[] = { g_service.stopEvent.get(), g_service.workEvent.get(), g_service.watcher.hProcess };
			const DWORD count = g_service.watcher.hProcess ? 3 : 2;
			const DWORD wait = WaitForMultipleObjects(count, handles, FALSE, MillisecondsUntilRetry());

			if (wait == WAIT_OBJECT_0 + 1) {
				// セッション変更と読み直しの要求が重なっても、置き直すのは 1 回だけ
				const bool sessionChanged = g_service.placeRequested.exchange(false);
				const bool reload = g_service.reloadRequested.exchange(false);
				if (sessionChanged) {
					rescue::Log(std::format(L"セッション変更: {}(セッション{}、コンソールセッション{})",
						PlacementEventName(g_service.lastSessionEvent), g_service.lastSessionId.load(), WTSGetActiveConsoleSessionId()));
				}
				if (reload) {
					rescue::Log(L"キーの割り当ての読み直しを要求されました");
				}
				if (sessionChanged || reload) {
					g_service.retryDelay = kRetryFirst;	// 状況が変わったので、間隔は最初から
					PlaceWatcher();
				}
				continue;
			}
			if (wait == WAIT_OBJECT_0 + 2) {
				OnWatcherExited();
				continue;
			}
			if (wait == WAIT_TIMEOUT) {
				PlaceWatcher();
				continue;
			}
			if (wait != WAIT_OBJECT_0) {
				rescue::Log(std::format(L"WaitForMultipleObjectsが失敗しました: {}", GetLastError()));
			}
			break;	//停止要求 or 待機失敗
		}

		// 待ち受け役は最大 kWatcherStopTimeout 待つので、SCM にはそれより長い見込み時間を伝えておく
		ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 10000);
		StopWatcher();	// サービスがいなくなった後に待ち受け役だけ残さない
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

bool rescue::service::RequestServiceReload()
{
	// コントロールを送るだけなので、SCM には接続だけ、サービスには SERVICE_USER_DEFINED_CONTROL だけを求める
	wil::unique_schandle manager{ OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT) };
	if (!manager) return false;
	wil::unique_schandle service{ OpenServiceW(manager.get(), kServiceName, SERVICE_USER_DEFINED_CONTROL) };
	if (!service) return false;

	SERVICE_STATUS status{};
	return ControlService(service.get(), kControlReload, &status) != FALSE;	// 止まっていると ERROR_SERVICE_NOT_ACTIVE で FALSE
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
