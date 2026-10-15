#include "service/ResidentProcess.h"

#include <wil/result.h>
#include <algorithm>
#include <format>

#include "common/Common.h"
#include "common/Log.h"

namespace {
	constexpr std::chrono::seconds kStopTimeout{ 5 };

	constexpr std::chrono::seconds kRetryFirst{ 5 };
	constexpr std::chrono::seconds kRetryMax{ 300 };
}

namespace rescue::service {
	ResidentProcess::ResidentProcess(std::wstring name, std::wstring command, Launcher launcher) : m_name{ std::move(name) }, m_command{ std::move(command) }, m_launcher{ std::move(launcher) }, m_retryDelay{ kRetryFirst } {
		m_stopEvent.create(wil::EventOptions::ManualReset);	//失敗すると例外
	}

	// 「止めて」を立てて kStopTimeout 待ち、終わらなければ強制終了する
	void ResidentProcess::Stop() {
		m_retryAt.reset();
		if (!m_process.hProcess) return;

		m_stopEvent.SetEvent();
		DWORD wait = WaitForSingleObject(m_process.hProcess, static_cast<DWORD>(std::chrono::milliseconds(kStopTimeout).count()));
		if (wait == WAIT_TIMEOUT) {
			rescue::Log(std::format(L"{}が止まらないため強制終了します。", m_name));
			TerminateProcess(m_process.hProcess, ERROR_TIMEOUT);
			WaitForSingleObject(m_process.hProcess, INFINITE);
		}

		DWORD exitCode = 0;
		GetExitCodeProcess(m_process.hProcess, &exitCode);
		rescue::Log(std::format(L"{}が終了:{}({})", m_name, rescue::DescribeError(static_cast<LONG>(exitCode)), exitCode));
		m_process.reset();
	}

	// 動いていれば止めてから起動する。起動のしかたは m_launcher（std::function）を呼ぶ
	void ResidentProcess::Start() {
		Stop();
		try {
			m_stopEvent.ResetEvent();

			// 子に渡すのは、待つことだけができる（SYNCHRONIZE）継承可能なコピー
			wil::unique_handle childStopEvent;
			THROW_IF_WIN32_BOOL_FALSE(DuplicateHandle(GetCurrentProcess(), m_stopEvent.get(), GetCurrentProcess(), childStopEvent.put(), SYNCHRONIZE, TRUE, 0));

			m_process = m_launcher(std::format(L"{} {}", m_command, HandleToULong(childStopEvent.get())), childStopEvent.get());
			rescue::Log(std::format(L"{}を起動(プロセス{},コンソールセッション{})", m_name, m_process.dwProcessId, WTSGetActiveConsoleSessionId()));
		}
		catch (...) {
			const HRESULT hr = wil::ResultFromCaughtException();
			rescue::Log(std::format(L"{}を起動できませんでした：{}", m_name, rescue::DescribeHresult(hr)));
			// ユーザーがいないときは何度試しても同じなので予定を入れない（サインインの通知を待つ）
			if (hr != HRESULT_FROM_WIN32(ERROR_NO_TOKEN)) ScheduleRetry();
		}
	}

	void ResidentProcess::OnExited() {
		DWORD exitCode = 0;
		GetExitCodeProcess(m_process.hProcess, &exitCode);
		rescue::Log(std::format(L"{}が自分で終了しました:{} ({})", m_name, rescue::DescribeError(static_cast<LONG>(exitCode)), exitCode));
		m_process.reset();
		ScheduleRetry();
	}

	// 現在の待機時間後に再起動を予約し、次回の待機時間を倍にする（上限 kRetryMax）
	void ResidentProcess::ScheduleRetry() {
		m_retryAt = std::chrono::steady_clock::now() + m_retryDelay;
		rescue::Log(std::format(L"{}秒後に{}を置きなおします", m_retryDelay.count(), m_name));
		m_retryDelay = (std::min)(m_retryDelay * 2, kRetryMax);
	}

	void ResidentProcess::ResetRetryDelay() noexcept {
		m_retryDelay = kRetryFirst;
	}

}
