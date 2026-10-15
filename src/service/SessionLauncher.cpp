#include "service/SessionLauncher.h"

#include <windows.h>
#include <userenv.h>        // CreateEnvironmentBlock（wil/resource.h より先に include すると wil::unique_environment_block が使える）
#include <wtsapi32.h>       // WTSQueryUserToken / WTSEnumerateProcessesW（wil/resource.h より先に include すると wil::unique_wtsmem_ptr が使える）
#include <wil/resource.h>   // wil::unique_handle / unique_environment_block / unique_process_information / unique_wtsmem_ptr
#include <wil/result.h>     // THROW_IF_WIN32_BOOL_FALSE など
#include <wil/stl.h>        // wil::GetModuleFileNameW を std::wstring で受けるため
#include <wil/win32_helpers.h>

#include <algorithm>
#include <filesystem>
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
	// program があれば、自分と同じフォルダの program: "C:\...\DisplayRescueMascot.exe" 420
	std::wstring BuildCommandLine(std::wstring_view arguments, std::wstring_view program) {
		std::filesystem::path exePath{ wil::GetModuleFileNameW<std::wstring>() };
		if (!program.empty()) exePath = exePath.parent_path() / program;
		return std::format(L"\"{}\" {}", exePath.wstring(), arguments);
	}

	//指定したセッションのwinlogon.exe(SYSTEMで動いている)のトークンを開く。
	// ユーザー不在のログイン画面にも winlogon.exe は存在するため、そのアクセストークンを複製する。
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

	//物理モニタがつながっているセッションの番号
	DWORD ConsoleSessionId() {
		const DWORD sessionId = WTSGetActiveConsoleSessionId();
		THROW_WIN32_IF(ERROR_NO_SUCH_LOGON_SESSION, sessionId == 0xFFFFFFFF);
		return sessionId;
	}

	// sourceToken を複製し、そのセキュリティコンテキストで desktop に "DisplayRescueService.exe <arguments>" を起動する。
	// uiAccessがtrueなら(3)と(4)の間でトークンにUIAccessを立てる
	wil::unique_process_information LaunchWithToken(HANDLE sourceToken, std::wstring desktop, std::wstring_view arguments, HANDLE inheritHandle, bool uiAccess, std::wstring_view program) {
		// 3. 自分専用のプライマリトークンにコピーする。アクセス権はCreateEnvironmentBlockとCreateProcessAsUserWが要求するものだけにする。
		// TOKEN_ADJUST_DEFAULTはSetTokenInformation(TokenUIAccess)に必要
		wil::unique_handle primaryToken;
		THROW_IF_WIN32_BOOL_FALSE(DuplicateTokenEx(
			sourceToken,
			TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | (uiAccess ? TOKEN_ADJUST_DEFAULT : 0),
			nullptr,				//新しいトークン自体のACL(誰がこのトークンを開けるか)。nullptrで既定
			SecurityImpersonation,	//偽装トークン(スレッドが一時的に他人になるためのトークン)の「偽装の強さ」。プライマリトークンでは使われない(慣例の値)
			TokenPrimary,
			primaryToken.put()));

		// トークンにUIAccessを立てる。
		// 普通は「uiAccess="true"のマニフェスト・署名・Program Files」を満たしたexeにAppInfoサービスがこの印をつける。
		// 印をつけること自体にはSeTcbPrivilegeが要る。LocalSystemのサービスは持っているので、確認を通さずに直接つけられる。
		// UIAccessの付いたプロセスのウィンドウはZBID_UIACCESSの層に入り、ロック画像(ZBID_LOCK)より上に出られる。
		// 起動するのは program で限った同じフォルダ（インストール先）の exe だけ。
		if (uiAccess) {
			DWORD enable = TRUE;
			THROW_IF_WIN32_BOOL_FALSE(SetTokenInformation(primaryToken.get(), TokenUIAccess, &enable, sizeof(enable)));
		}

		// 4. そのユーザーの環境変数(USERPROFILE,TEMPなど)を作る。
		// 作らないと起動したプロセスはサービス(SYSTEM)の環境変数を引き継いでしまう。
		wil::unique_environment_block environment;
		THROW_IF_WIN32_BOOL_FALSE(CreateEnvironmentBlock(environment.put(), primaryToken.get(), FALSE));

		// 5. 起動する。lpCommandLineはAPIが解析中に一時的に書き換えることがあるので、
		//  文字列リテラル(読み取り専用)ではなく書き換え可能なバッファ(std::wstring)を渡す。
		std::wstring commandLine = BuildCommandLine(arguments, program);

		STARTUPINFOEXW startup{};
		//cbには構造体の大きさを入れる。STARTUPINFOEXWはSTARTUPINFOWの後ろにlpAttributeListを足した大きい版で、
		//OSはcbがEX版の大きさで、かつflagsにEXTENDED_STARTUPINFO_PRESENTがあるときだけlpAttributeListまで読む
		startup.StartupInfo.cb = sizeof(startup);
		startup.StartupInfo.lpDesktop = desktop.data();
		DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;

		// 6. 継承させるハンドルがあれば「属性リスト」で、子に渡すのをそれ一つだけに絞る
		//CreateProcessAsUserWのbInheritHandles=TRUEだけだと、サービスの継承可能なハンドルが全部子にわたってしまう。
		//属性リストは大きさが決まっていない構造体なので大きさを聞く→大きさ分のバッファを用意して初期化
		std::vector<std::byte> attributeBuffer;
		LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
		const auto deleteAttributes = wil::scope_exit([&] {if (attributes) DeleteProcThreadAttributeList(attributes); });
		if (inheritHandle) {
			SIZE_T size = 0;
			InitializeProcThreadAttributeList(nullptr, 1, 0, &size);	//大きさを聞くだけ。ERROR_INSUFFICIENT_BUFFERで失敗するのが正常。
			attributeBuffer.resize(size);
			THROW_IF_WIN32_BOOL_FALSE(InitializeProcThreadAttributeList(reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data()), 1, 0, &size));
			attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());

			//&inheritHandle(値の置き場所)を渡す。リストは中身をコピーせずアドレスを覚えるだけで、CreateProcessAsUserWがそのとき読みに行く。
			//だからCreateProcessAsUserWまで生きている変数であること(ここは関数の引数なので最後まで生きている)
			THROW_IF_WIN32_BOOL_FALSE(
				UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inheritHandle, sizeof(inheritHandle), nullptr, nullptr)
			);

			startup.lpAttributeList = attributes;
			flags |= EXTENDED_STARTUPINFO_PRESENT;	//flags = flags | X の略。そのフラグのビットを立てる
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

namespace rescue::service {

	wil::unique_process_information StartInInputDesktop(std::wstring_view arguments, HANDLE inheritHandle, std::wstring_view program)
	{
		// 1. 物理モニタがつながっているセッションの番号
		const DWORD sessionId = ConsoleSessionId();

		// 2. 借りるトークンと起動先のデスクトップを決める。
		// ロック中は、ユーザーの有無にかかわらず winlogon.exe のアクセストークン（SYSTEM）で Winlogon デスクトップへ起動する。
		// それ以外は、サインイン中のユーザーのアクセストークンで Default デスクトップへ起動する。
		// ユーザー不在（ログイン画面・RDP）では WTSQueryUserToken が ERROR_NO_TOKEN (1008) を返すため、その場合だけ winlogon.exe のアクセストークンで Winlogon デスクトップへ起動する。
		wil::unique_handle sourceToken;
		std::wstring desktop;
		if (rescue::IsSessionLocked(sessionId)) {
			rescue::Log(L"ロック中のため、winlogonのアクセストークンでWinlogonデスクトップに起動します");
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

			rescue::Log(L"ログオン中のユーザーがいないため、winlogonのアクセストークンでWinlogonデスクトップに起動します");
			sourceToken = OpenWinlogonToken(sessionId);
			desktop = L"winsta0\\Winlogon";
		}

		// (3)〜(6)
		return LaunchWithToken(sourceToken.get(), std::move(desktop), arguments, inheritHandle, false, program);
	}

	// ロック状態に関係なく、ユーザーのアクセストークンで Default に UIAccess 付きで起動する
	wil::unique_process_information StartInUserDesktopWithUiAccess(std::wstring_view arguments, HANDLE inheritHandle, std::wstring_view program)
	{
		const DWORD sessionId = ConsoleSessionId();

		wil::unique_handle sourceToken;
		THROW_IF_WIN32_BOOL_FALSE(WTSQueryUserToken(sessionId, sourceToken.put()));	//ユーザーがいなければ ERROR_NO_TOKEN

		return LaunchWithToken(sourceToken.get(), L"winsta0\\default", arguments, inheritHandle, true, program);
	}

	// ロック状態に関係なく、winlogon のアクセストークンで Winlogon に起動する（StartInInputDesktop のロック中と同じ処理）
	wil::unique_process_information StartInWinlogonDesktop(std::wstring_view arguments, HANDLE inheritHandle, std::wstring_view program)
	{
		const DWORD sessionId = ConsoleSessionId();
		const wil::unique_handle sourceToken = OpenWinlogonToken(sessionId);
		return LaunchWithToken(sourceToken.get(), L"winsta0\\Winlogon", arguments, inheritHandle, false, program);
	}

	// コンソールセッションがロック中か（StartInInputDesktop と同じ判定）。マスコットをロック中だけ置くため
	bool IsConsoleSessionLocked()
	{
		return rescue::IsSessionLocked(ConsoleSessionId());
	}
}
