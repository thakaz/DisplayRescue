#include "common/Log.h"

#include <windows.h>
#include <aclapi.h>          // GetNamedSecurityInfoW / SetNamedSecurityInfoW
#include <sddl.h>            // ConvertStringSecurityDescriptorToSecurityDescriptorW
#include <ShlObj.h>          // SHGetKnownFolderPath
#include <wil/resource.h>    // wil::unique_cotaskmem_string
#include <wil/result.h>      // wil::SetResultLoggingCallback, wil::GetFailureLogString

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>

namespace {

	std::mutex g_logMutex;

	// これを超えたら rescue.log を rescue.log.1 に回して新しく書き始める（残すのは 1 世代だけ。最大でおよそ 2 倍の容量）
	constexpr std::uintmax_t kMaxLogBytes = 1024 * 1024;

	// C:\ProgramData\DisplayRescue\rescue.log
	// Programdataはユーザーに依存しない。
	std::filesystem::path LogFilePath() {
		//put()は「中身を解放してから書き込み先のアドレスを返す」。
		wil::unique_cotaskmem_string programData;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, programData.put()))) {
			return{};
		}
		return std::filesystem::path{ programData.get() } / L"DisplayRescue" / L"rescue.log";
	}

	// ログのフォルダの ACL（SDDL という文字列の書き方）。
	//   O:BA          持ち主は Administrators
	//   D:P           親(ProgramData)の ACL を引き継がない。ProgramData の下は一般ユーザーもファイルを作れるので、それを断ち切る
	//   (A;OICI;FA;;;SY) SYSTEM はフルコントロール。OICI = 中のファイルとフォルダにも引き継ぐ
	//   (A;OICI;FA;;;BA) Administrators もフルコントロール
	//   (A;OICI;FRFX;;;BU) Users は読み取りと実行だけ（ログを読めるように）
	constexpr wchar_t kLogDirSddl[] = L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)";

	// kLogDirSddl をセキュリティ記述子にする。失敗したら空。
	// Log の中からも呼ぶので例外にしない（WIL の例外はログに流れ、Log をもう一度呼んでしまう）
	wil::unique_hlocal_security_descriptor LogDirectorySecurity() noexcept {
		wil::unique_hlocal_security_descriptor sd;
		ConvertStringSecurityDescriptorToSecurityDescriptorW(kLogDirSddl, SDDL_REVISION_1, wil::out_param(sd), nullptr);
		return sd;
	}

	// 持ち主が SYSTEM か Administrators か（＝一般ユーザーが作ったものではないか）
	bool IsOwnedByAdminOrSystem(const std::filesystem::path& path) noexcept {
		PSID owner = nullptr;	// sd の中を指すので、sd より長く使わない
		wil::unique_hlocal_security_descriptor sd;
		if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, wil::out_param(sd)) != ERROR_SUCCESS) {
			return false;
		}
		return IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid);
	}

	// リンク（再解析ポイント）ではない本物のフォルダで、持ち主が SYSTEM か Administrators か
	bool IsTrustedDirectory(const std::filesystem::path& dir) noexcept {
		const DWORD attributes = GetFileAttributesW(dir.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES
			&& (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
			&& (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0
			&& IsOwnedByAdminOrSystem(dir);
	}

	// ログを書いてよいフォルダか確かめる。無ければ管理者と SYSTEM だけが書ける ACL で作る。
	// 一般ユーザーの身分では持ち主を Administrators にできないので作れない（＝ログは書かない）
	bool PrepareLogDirectory(const std::filesystem::path& dir) noexcept {
		if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
			const auto sd = LogDirectorySecurity();
			if (!sd) return false;
			SECURITY_ATTRIBUTES sa{ .nLength = sizeof(sa), .lpSecurityDescriptor = sd.get(), .bInheritHandle = FALSE };
			return CreateDirectoryW(dir.c_str(), &sa) != FALSE;
		}
		return IsTrustedDirectory(dir);
	}

	std::string ToUtf8(std::wstring_view text) {
		if (text.empty()) return{};

		const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		std::string utf8(static_cast<size_t>(size), '\0');
		WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
		return utf8;

	}

	//WILが失敗を検出するたびに呼ばれる。
	void __stdcall OnWilFailure(const wil::FailureInfo& failure) noexcept {
		// 例: "Service.cpp(123)\DisplayRescueService.exe!...: (caller: ...) Exception(1) tid(...) 80070005 アクセスが拒否されました。"
		wchar_t message[2048]{};
		wil::GetFailureLogString(message, std::size(message), failure);

		std::wstring_view text{ message };
		while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) {
			text.remove_suffix(1);
		}
		rescue::Log(std::format(L"[WIL] {}", text));
	}

}

namespace rescue {

	void Log(std::wstring_view message) noexcept
	{
		try {
			SYSTEMTIME t{};
			GetLocalTime(&t);

			DWORD sessionId = 0;
			ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);

			const std::wstring line = std::format(
				L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [pid {} / セッション{}] {}\r\n",
				t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
				GetCurrentProcessId(), sessionId, message);

			const std::scoped_lock lock{ g_logMutex };

			static const std::filesystem::path path = LogFilePath();
			// SYSTEM が一般ユーザーの用意したフォルダ（リンクを仕込まれているかもしれない）に書き込まないよう、
			// 使ってよいフォルダかをプロセスで最初の 1 回だけ確かめる
			static const bool usable = !path.empty() && PrepareLogDirectory(path.parent_path());
			if (!usable) return;

			std::error_code ec;

			// 大きくなりすぎたら 1 世代前として退避する（前の rescue.log.1 は上書き）。失敗しても書くのは続ける
			if (const auto size = std::filesystem::file_size(path, ec); !ec && size > kMaxLogBytes) {
				auto previous = path;
				previous += L".1";
				std::filesystem::rename(path, previous, ec);
			}

			// 新しく作るときだけ先頭に UTF-8 の BOM を付ける。
			// BOM がないと Windows PowerShell 5.1 の Get-Content が日本語を文字化けさせる
			const bool isNew = !std::filesystem::exists(path, ec);

			std::ofstream out(path, std::ios::app | std::ios::binary);
			if (isNew) out << "\xEF\xBB\xBF";
			out << ToUtf8(line);
		}
		catch (...) {
			//握りつぶす
		}
	
	}

	void RouteWilFailuresToLog()
	{
		wil::SetResultLoggingCallback(OnWilFailure);
	}

	void SecureLogDirectory()
	{
		const auto dir = LogFilePath().parent_path();
		THROW_HR_IF(E_UNEXPECTED, dir.empty());
		const auto sd = LogDirectorySecurity();
		THROW_LAST_ERROR_IF(!sd);

		// 1. あるのに信頼できない（一般ユーザーが先に作った・リンクになっている）なら、中には触らず名前を変えて脇に退ける
		if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES && !IsTrustedDirectory(dir)) {
			auto aside = dir;
			aside += std::format(L".untrusted-{}", GetTickCount64());
			THROW_IF_WIN32_BOOL_FALSE(MoveFileExW(dir.c_str(), aside.c_str(), 0));
		}

		// 2. 無ければ、管理者と SYSTEM だけが書ける ACL で作って終わり
		if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
			SECURITY_ATTRIBUTES sa{ .nLength = sizeof(sa), .lpSecurityDescriptor = sd.get(), .bInheritHandle = FALSE };
			THROW_IF_WIN32_BOOL_FALSE(CreateDirectoryW(dir.c_str(), &sa));
			return;
		}

		// 3. 自分たち（SYSTEM か管理者）のフォルダ。持ち主と ACL を締め直す。引き継ぐ ACE は中のファイルにも反映される
		PSID owner = nullptr;
		PACL dacl = nullptr;
		BOOL present = FALSE;
		BOOL defaulted = FALSE;
		THROW_IF_WIN32_BOOL_FALSE(GetSecurityDescriptorOwner(sd.get(), &owner, &defaulted));
		THROW_IF_WIN32_BOOL_FALSE(GetSecurityDescriptorDacl(sd.get(), &present, &dacl, &defaulted));
		THROW_IF_WIN32_ERROR(SetNamedSecurityInfoW(
			const_cast<LPWSTR>(dir.c_str()), SE_FILE_OBJECT,
			OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
			owner, nullptr, dacl, nullptr));

		// 4. 締める前に一般ユーザーが置いたかもしれないもの（リンク、一般ユーザーが持ち主のファイル）を取り除く。
		//    リンクは「リンク自体」を消すだけで、指している先には触らない
		for (const auto& entry : std::filesystem::directory_iterator(dir)) {
			const auto& path = entry.path();
			const DWORD attributes = GetFileAttributesW(path.c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES) continue;
			if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 && IsOwnedByAdminOrSystem(path)) continue;

			if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
				RemoveDirectoryW(path.c_str());
			}
			else {
				DeleteFileW(path.c_str());
			}
		}
	}


}