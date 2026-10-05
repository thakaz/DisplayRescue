#include "service/SessionLauncher.h"

#include <windows.h>
#include <userenv.h>        // CreateEnvironmentBlock（wil/resource.h より先に include すると wil::unique_environment_block が使える）
#include <wtsapi32.h>       // WTSQueryUserToken / WTSEnumerateProcessesW（wil/resource.h より先に include すると wil::unique_wtsmem_ptr が使える）
#include <wil/resource.h>   // wil::unique_handle / unique_environment_block / unique_process_information / unique_wtsmem_ptr
#include <wil/result.h>     // THROW_IF_WIN32_BOOL_FALSE など
#include <wil/stl.h>        // wil::GetModuleFileNameW を std::wstring で受けるため
#include <wil/win32_helpers.h>

#include <algorithm>
#include <format>
#include <span>
#include <string>
#include <vector>

#include "common/Common.h"
#include "common/Log.h"

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

namespace {
	// 自分自身のexeを起動するコマンドライン: "C:\...\DisplayRescueService.exe" watch 420
	// パスに空白があっても切れないよう""で囲む。
	std::wstring BuildCommandLine(std::wstring_view arguments) {
		const auto exePath = wil::GetModuleFileNameW<std::wstring>();
		return std::format(L"\"{}\" {}", exePath, arguments);
	}

	//指定したセッションのwinlogon.exe(SYSTEMで動いている)のトークンを開く。
	//ユーザーがいないログイン画面でもwinlogon.exeが必ずいるので、その身分を借りる。
	wil::unique_handle OpenWinlogonToken(DWORD sessionId) {
		//(a)セッション番号付きのプロセス一覧をもらう。一覧のメモリはWTSFreeMemoryで返す必要があるので箱に入れる
		wil::unique_wtsmem_ptr<WTS_PROCESS_INFOW> processes;
		DWORD count = 0;

		THROW_IF_WIN32_BOOL_FALSE(WTSEnumerateProcessesW(WTS_CURRENT_SERVER_HANDLE, 0, 1, wil::out_param(processes), &count));

		//一覧の中から「そのセッションのwinlogon.exe」を探す。spanは「先頭アドレス+個数」を配列のように扱うための型
		const std::span list{ processes.get(),count };
		const auto it = std::ranges::find_if(list, [sessionId](const WTS_PROCESS_INFOW& p) {return p.SessionId == sessionId && p.pProcessName != nullptr && rescue::EqualsIgnoreCase(p.pProcessName, L"winlogon.exe"); });

		THROW_WIN32_IF(ERROR_NOT_FOUND, it == list.end());

		//(b)プロセスを開く。トークンを開くだけなので弱い権限でOK
		wil::unique_handle process{
			OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,it->ProcessId)
		};

		THROW_LAST_ERROR_IF_NULL(process);

		//(c)トークンを開く。この後DuplicateTokenExでコピーするのでコピーと問い合わせの権限だけ。
		wil::unique_handle token;
		THROW_IF_WIN32_BOOL_FALSE(OpenProcessToken(process.get(), TOKEN_DUPLICATE | TOKEN_QUERY, token.put()));

		return token;
	}

	//ロック中か。WTSSessionInfoEx の SessionFlags で調べる
	//(Windows 7 / Server 2008 R2 は LOCK と UNLOCK が逆になる不具合があるが、対象外)
	bool IsSessionLocked(DWORD sessionId) {
		//バッファの型は LPWSTR として宣言されているが、WTSSessionInfoEx を頼むと中身は WTSINFOEXW になる。
		//WTSFreeMemory で返す必要があるので、受け取ったらすぐ箱に入れる
		LPWSTR buffer = nullptr;
		DWORD bytes = 0;
		THROW_IF_WIN32_BOOL_FALSE(WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId, WTSSessionInfoEx, &buffer, &bytes));
		const wil::unique_wtsmem_ptr<WTSINFOEXW> info{ reinterpret_cast<WTSINFOEXW*>(buffer) };

		THROW_WIN32_IF(ERROR_INVALID_DATA, info->Level != 1);	//今の Windows は Level 1 しか返さない
		return info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
	}
}

namespace rescue::service {

	wil::unique_process_information StartInInputDesktop(std::wstring_view arguments, HANDLE inheritHandle)
	{
		//①物理モニタがつながっているセッションの番号。切替中などでないときは0xFFFFFFFF
		const DWORD sessionId = WTSGetActiveConsoleSessionId();
		THROW_WIN32_IF(ERROR_NO_SUCH_LOGON_SESSION, sessionId == 0xFFFFFFFF);

		//②借りるトークンと起動先のデスクトップを決める。
		// ロック中なら、ユーザーがいても winlogon.exeの身分(SYSTEM)でWinlogonデスクトップへ(ロック画面はWinlogonデスクトップ。ユーザー不在のログイン画面もロック中と報告される)。
		// それ以外は、ユーザーがいればそのユーザーの身分でDefaultデスクトップへ
		// ユーザーがいない（ログイン画面・RDP)とWTSQueryUserTokenがERROR_NO_TOKEN(1008)になるので、その時だけwinlogon.exeの身分(SYSTEM)でWinlogonデスクトップへ。
		wil::unique_handle sourceToken;
		std::wstring desktop;
		if (IsSessionLocked(sessionId)) {
			rescue::Log(L"ロック中のため、winlogonの身分でWinlogonデスクトップに起動します");
			sourceToken = OpenWinlogonToken(sessionId);
			desktop = L"winsta0\\Winlogon";
		}
		else if (WTSQueryUserToken(sessionId, sourceToken.put())) {
			desktop = L"winsta0\\default";
		}
		else {
			//ERROR_NO_TOKEN以外の失敗は想定外なのでそのまま例外にする。
			const DWORD error = GetLastError();
			THROW_WIN32_IF(error, error != ERROR_NO_TOKEN);

			rescue::Log(L"ログオン中のユーザーがいないため、winlogonの身分でWinlogonデスクトップに起動します");
			sourceToken = OpenWinlogonToken(sessionId);
			desktop = L"winsta0\\Winlogon";
		}

		//③自分専用のプライマリトークンにコピーする。アクセス権はCreateEnvironmentBlockとCreateProcessAsUserWが要求するものだけにする
		wil::unique_handle primaryToken;
		THROW_IF_WIN32_BOOL_FALSE(DuplicateTokenEx(
			sourceToken.get(),
			TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY,
			nullptr,                //新しいトークン自体のACL(誰がこのトークンを開けるか)。nullptrで既定
			SecurityImpersonation,  //偽装トークンの「偽装の強さ」。プライマリトークンでは使われない(慣例の値)
			TokenPrimary,
			primaryToken.put()));

		//④そのユーザーの環境変数(USERPROFILE,TEMPなど)を作る。
		// 作らないとエージェントはサービス(SYSTEM)の環境変数を引き継いでしまう。
		wil::unique_environment_block environment;
		THROW_IF_WIN32_BOOL_FALSE(CreateEnvironmentBlock(environment.put(), primaryToken.get(), FALSE));

		//⑤起動する。lpCommandLineはAPIが解析中に一時的に書き換えることがあるので、
		//  文字列リテラル(読み取り専用)ではなく書き換え可能なバッファ(std::wstring)を渡す。
		std::wstring commandLine = BuildCommandLine(arguments);

		STARTUPINFOEXW startup{};
		startup.StartupInfo.cb = sizeof(startup);	//cbは構造体の版を伝える欄。EX版の大きさとEXTENDED_STARTUPINFO_PRESENTがそろうと、OSはlpAttributeListまで読む
		startup.StartupInfo.lpDesktop = desktop.data();
		DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;

		//⑥継承させるハンドルがあれば「属性リスト」でそれ一つだけに絞る
		// CreateProcessAsUserWのbInheritHandles=TRUEだけだと、サービスの継承可能なハンドルが全部子にわたってしまう。
		// 属性リストは大きさが決まっていない構造体なので大きさを聞く→大きさ分のバッファを用意して初期化
		std::vector<std::byte> attributeBuffer;
		LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
		const auto deleteAttributes = wil::scope_exit([&] {if (attributes) DeleteProcThreadAttributeList(attributes); });
		if (inheritHandle) {
			SIZE_T size = 0;
			InitializeProcThreadAttributeList(nullptr, 1, 0, &size);	//大きさを聞くだけ。ERROR_INSUFFICIENT_BUFFERで失敗するのが正常。
			attributeBuffer.resize(size);
			THROW_IF_WIN32_BOOL_FALSE(InitializeProcThreadAttributeList(reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data()), 1, 0, &size));
			attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());

			//inheritHandleの場所を渡す。リストはこのアドレスを覚えるだけなので、CreateProcessAsUserWまで生きている変数であるこど。
			THROW_IF_WIN32_BOOL_FALSE(
				UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inheritHandle, sizeof(inheritHandle), nullptr, nullptr)
			);

			startup.lpAttributeList = attributes;
			flags |= EXTENDED_STARTUPINFO_PRESENT;
		}

		wil::unique_process_information process;

		THROW_IF_WIN32_BOOL_FALSE(CreateProcessAsUserW(
			primaryToken.get(),
			nullptr,                //実行ファイルはコマンドラインの先頭(" で囲んだ部分)から決まる
			commandLine.data(),
			nullptr, nullptr,       //新しいプロセス・スレッドのセキュリティ属性。nullptrで既定
			inheritHandle != nullptr,	//継承させるのは属性リストに入れたハンドルだけ。無ければ何も継承させない
			flags,
			environment.get(),
			nullptr,                // カレントディレクトリ
			&startup.StartupInfo,
			&process));

		return process;
	}
}
