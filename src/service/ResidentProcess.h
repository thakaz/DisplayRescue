#pragma once

#include <windows.h>
#include <wil/resource.h>	// wil::unique_event / unique_process_information

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace rescue::service {
	// サービスが置きっぱなしにする子プロセス 1 つ。
	// ホットキー監視プロセス・マスコット（パスワード欄用）・マスコット（ロック画像用）の 3 つに使う。違うのは名前・サブコマンド・起動方法だけ
	//
	// 子には「止めて」のイベントを渡し、"<command> <ハンドルの値>" で起動する（watch と同じ）。
	// 止めるときはイベントを立てて kStopTimeout 待ち、終わらなければ強制終了する。
	// 起動に失敗した場合やプロセスが終了した場合は、5 秒から倍増する間隔（上限 5 分）で再起動を予約する。
	// 予定の時刻はサービスのループが RetryAt で見て、過ぎたら Start を呼ぶ
	class ResidentProcess {
	public:
		// 起動のしかた。SessionLauncher の StartInInputDesktop / StartInWinlogonDesktop / StartInUserDesktopWithUiAccess と同じ形
		using Launcher = std::function<wil::unique_process_information(std::wstring_view arguments, HANDLE inheritHandle)>;

		// name: ログに出す名前（「ホットキー監視プロセス」など）。command: 起動するサブコマンド（"watch"。別の exe を起動するときは空）。
		// 「止めて」のイベントをここで作る（失敗は例外）
		ResidentProcess(std::wstring name, std::wstring command, Launcher launcher);

		// 子プロセスとイベントを 1 つずつ持つので、コピーはできない（2 つが同じ子を止めたり待ったりしないように）
		ResidentProcess(const ResidentProcess&) = delete;
		ResidentProcess& operator=(const ResidentProcess&) = delete;

		// 起動する。動いていれば止めてから起動し直す（ホットキーと同じく、同じ役が 2 つ同時にいると困るため）。
		// 起動に失敗した場合は再起動を予約する。ただし ERROR_NO_TOKEN（ユーザー不在）の場合は予約せず、サインイン通知を待つ
		void Start();

		// 停止する。動作していなければ何もしない。再起動の予約も解除する
		void Stop();

		// 子プロセスが終了し、ProcessHandle がシグナル状態になったときに呼ぶ。終了コードを記録して再起動を予約する
		void OnExited();

		bool IsRunning() const noexcept { return m_process.hProcess != nullptr; }
		HANDLE ProcessHandle() const noexcept { return m_process.hProcess; }	// 動いていなければ nullptr
		const std::wstring& Name() const noexcept { return m_name; }

		// 再起動予定の時刻。予定がなければ nullopt
		std::optional<std::chrono::steady_clock::time_point> RetryAt() const noexcept { return m_retryAt; }

		// 再起動間隔を初期値（5 秒）に戻す。セッション変更など、状況が変化したときに使う
		void ResetRetryDelay() noexcept;

	private:
		void ScheduleRetry();

		std::wstring m_name;
		std::wstring m_command;
		Launcher m_launcher;
		wil::unique_event m_stopEvent;				// 子への「止めて」（手動リセット）
		wil::unique_process_information m_process;	// 動いていなければ hProcess が nullptr
		std::chrono::seconds m_retryDelay;			// 次回の再起動までの間隔
		std::optional<std::chrono::steady_clock::time_point> m_retryAt;
	};
}
