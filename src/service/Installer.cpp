#include "service/Installer.h"

#include <windows.h>
#include <ShlObj.h>         // SHGetKnownFolderPath
#include <wil/resource.h>   // wil::unique_schandle / unique_cotaskmem_string
#include <wil/result.h>     // THROW_IF_WIN32_BOOL_FALSE など
#include <wil/stl.h>        // wil::GetModuleFileNameW を std::wstring で受けるため
#include <wil/win32_helpers.h>

#include <chrono>
#include <format>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>

#include "common/Log.h"
#include "config/Settings.h"
#include "service/Service.h"

using namespace std::chrono_literals;

namespace {
	using namespace rescue::service;
	namespace fs = std::filesystem;

	constexpr wchar_t kDisplayName[] = L"DisplayRescue";

	// インストール先に置く exe（サービスが動かす）
	constexpr wchar_t kServiceExe[] = L"DisplayRescueService.exe";

	//サービスの操作に使う権限。止める・状態を見る・設定を変える・起動する だけ。
	constexpr DWORD kServiceAccess = SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG | SERVICE_START;

	// サービスを動かすアカウント。CreateServiceW では nullptr でもこれになるが、ChangeServiceConfigW の nullptr は「今のまま」なので明示する
	constexpr wchar_t kServiceAccount[] = L"LocalSystem";

	fs::path KnownFolder(REFKNOWNFOLDERID id) {
		wil::unique_cotaskmem_string folder;
		THROW_IF_FAILED(SHGetKnownFolderPath(id, 0, nullptr, folder.put()));
		return fs::path{ folder.get() };
	}

	fs::path SelfPath() {
		return fs::path{ wil::GetModuleFileNameW<std::wstring>() };
	}

	bool SameFile(const fs::path& a, const fs::path& b) {
		std::error_code ec;
		return fs::equivalent(a, b, ec);
	}

	//サービスが動いていれば止め、STOPPEDになるまで待つ。
	void StopServiceAndWait(SC_HANDLE service, std::chrono::seconds timeout) {
		SERVICE_STATUS status{};
		if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
			const DWORD error = GetLastError();
			if (error == ERROR_SERVICE_NOT_ACTIVE) return; //もともと止まってる。

			THROW_WIN32(error);
		}

		const auto deadline = std::chrono::steady_clock::now() + timeout;
		for (;;) {
			SERVICE_STATUS_PROCESS current{};
			DWORD needed = 0;
			THROW_IF_WIN32_BOOL_FALSE(QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&current), sizeof(current), &needed));
			if (current.dwCurrentState == SERVICE_STOPPED) return;

			THROW_WIN32_IF(ERROR_TIMEOUT, std::chrono::steady_clock::now() > deadline);

			std::this_thread::sleep_for(200ms);
		}
	}

	// ファイルが使用中のエラーか。動いている exe は上書きしようとすると ERROR_SHARING_VIOLATION、消そうとすると ERROR_ACCESS_DENIED
	bool IsInUseError(DWORD error) {
		return error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED;
	}

	// action() を呼び、ファイルが使用中で失敗している間だけ、少し待って試し直す（最大 200ms × 25）。
	// action() は成功したら true、失敗したら false を返す関数。失敗理由は GetLastError
	void RetryWhileInUse(auto action) {
		for (int attempt = 0;; ++attempt) {
			if (action()) return;

			const DWORD error = GetLastError();
			THROW_WIN32_IF(error, !IsInUseError(error) || attempt >= 25);
			std::this_thread::sleep_for(200ms);
		}
	}

	// 配布フォルダ（今動いている exe と同じフォルダ）の DisplayRescueService.exe を target にコピーする。
	// インストール先の exe から install したときは、コピー元とコピー先が同じなので何もしない。配布フォルダに無ければ ERROR_FILE_NOT_FOUND
	void CopyServiceExe(const fs::path& target, const Progress& progress) {
		const fs::path source = SelfPath().parent_path() / kServiceExe;
		if (SameFile(source, target)) return;

		std::error_code ec;
		THROW_WIN32_IF(ERROR_FILE_NOT_FOUND, !fs::exists(source, ec));

		//フォルダを作る。Program FilesのACL(管理者とSYSTEMだけが書ける)がそのまま引き継がれる。
		fs::create_directories(target.parent_path(), ec);
		THROW_WIN32_IF(static_cast<DWORD>(ec.value()), static_cast<bool>(ec));

		// サービスが STOPPED になってもプロセスが exe を手放すまで一瞬かかることがある
		RetryWhileInUse([&] { return CopyFileW(source.c_str(), target.c_str(), FALSE) != FALSE; });
		progress(std::format(L"コピーしました: {}", target.wstring()));
	}

	//サービスを登録する。すでにあれば設定を上書きする。
	wil::unique_schandle CreateOrUpdateService(SC_HANDLE manager, const std::wstring& command) {
		wil::unique_schandle service{ CreateServiceW(
			manager,
			kServiceName,
			kDisplayName,
			kServiceAccess,
			SERVICE_WIN32_OWN_PROCESS,		//1プロセス1サービス
			SERVICE_AUTO_START,				//Windowsの起動時、ログインより前に起動する。
			SERVICE_ERROR_NORMAL,			//起動に失敗したらイベントログに残す(Windowsの起動は続ける)
			command.c_str(),
			nullptr,nullptr,nullptr,		//起動順のグループ・タグ・依存サービスはなし
			kServiceAccount,
			nullptr) };						//パスワード（LocalSystem には無い）

		if (service) return service;

		const DWORD error = GetLastError();
		THROW_WIN32_IF(error, error != ERROR_SERVICE_EXISTS);

		// すでにある⇒ 開いてパス・起動の種類・アカウントを上書きする。
		// SERVICE_NO_CHANGE / nullptrを渡した項目は今の値のまま。
		service.reset(OpenServiceW(manager, kServiceName, kServiceAccess));
		THROW_LAST_ERROR_IF_NULL(service);
		THROW_IF_WIN32_BOOL_FALSE(ChangeServiceConfigW(
			service.get(),
			SERVICE_WIN32_OWN_PROCESS,
			SERVICE_AUTO_START,
			SERVICE_ERROR_NORMAL,
			command.c_str(),
			nullptr, nullptr, nullptr,
			kServiceAccount, L"",			//パスワード。LocalSystem に変えるときは空文字列を渡す（nullptr は「今のまま」）
			kDisplayName));

		return service;
	}

	//説明文と異常終了した時の動き(回復オプション)を設定
	void ConfigureService(SC_HANDLE service) {
		//lpDescriptionは書き換え可能なLPWSTR(const が付いていない)なので、リテラルではなく配列に入れて渡す
		wchar_t descriptionText[] = L"画面が映らないときでも、ログイン画面・ロック画面を含めて、割り当てたキー(既定はCtrl+Alt+F8〜F11)でディスプレイ構成を切り替えます。";
		SERVICE_DESCRIPTIONW description{ .lpDescription = descriptionText };
		THROW_IF_WIN32_BOOL_FALSE(ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description));

		//異常終了したら5秒後に再起動する(max3times)
		SC_ACTION actions[] = {
			{ SC_ACTION_RESTART,5000 },
			{ SC_ACTION_RESTART,5000 },
			{ SC_ACTION_RESTART,5000 },
		};
		SERVICE_FAILURE_ACTIONSW failure{
			.dwResetPeriod = 24 * 60 * 60, //１日でリセット
			.cActions = static_cast<DWORD>(std::size(actions)),
			.lpsaActions = actions,
		};
		THROW_IF_WIN32_BOOL_FALSE(ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure));
	}

	// ファイルを消す。インストール先の exe 自身から uninstall したときは今は消せないので、次の再起動で消えるよう予約する（管理者だけができる）。
	// 予約したら true。もともと無ければ何もせず false
	bool DeleteOrScheduleOnReboot(const fs::path& file) {
		std::error_code ec;
		if (!fs::exists(file, ec)) return false;

		if (SameFile(file, SelfPath())) {
			THROW_IF_WIN32_BOOL_FALSE(MoveFileExW(file.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT));
			return true;
		}
		// 止めた直後のサービスのプロセスが exe を手放すまで少し待つ
		RetryWhileInUse([&] { return DeleteFileW(file.c_str()) != FALSE || GetLastError() == ERROR_FILE_NOT_FOUND; });
		return false;
	}
}

fs::path rescue::service::InstallDirectory() {
	// 64 ビットのプロセスから FOLDERID_ProgramFiles を開くと "C:\Program Files" が返る
	return KnownFolder(FOLDERID_ProgramFiles) / L"DisplayRescue";
}

std::optional<rescue::FileVersion> rescue::service::InstalledVersion() {
	return ReadFileVersion(InstallDirectory() / kServiceExe);
}

void rescue::service::Install(const Progress& progress)
{
	// 1. SCM に接続する。管理者でない場合はここで ERROR_ACCESS_DENIED
	wil::unique_schandle manager{ OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE) };
	THROW_LAST_ERROR_IF_NULL(manager);

	// 2. ログのフォルダを管理者と SYSTEM だけが書ける状態にする（SYSTEM のサービスがここに書くため）
	rescue::SecureLogDirectory();

	// 3. すでにサービスがあれば止める（動いている exe は上書きできない）
	if (wil::unique_schandle existing{ OpenServiceW(manager.get(), kServiceName, kServiceAccess) }; existing) {
		progress(L"動いているサービスを停止しています...");
		StopServiceAndWait(existing.get(), 30s);
	}
	else {
		const DWORD error = GetLastError();
		THROW_WIN32_IF(error, error != ERROR_SERVICE_DOES_NOT_EXIST);
	}

	// 4. 配布フォルダの DisplayRescueService.exe を Program Files にコピーする
	const auto target = InstallDirectory() / kServiceExe;
	CopyServiceExe(target, progress);

	// 5. サービスを登録し、説明文と回復オプションを設定して起動する。パスは " で囲む（空白を含むため）
	const auto service = CreateOrUpdateService(manager.get(), std::format(L"\"{}\" service", target.wstring()));
	ConfigureService(service.get());
	if (!StartServiceW(service.get(), 0, nullptr)) {
		const DWORD error = GetLastError();
		THROW_WIN32_IF(error, error != ERROR_SERVICE_ALREADY_RUNNING);
	}
	progress(L"サービスを登録して起動しました(Windowsの起動時に自動で起動します)");
}

void rescue::service::Uninstall(const Progress& progress)
{
	// 1. SCM に接続する。サービスを開くだけなので SC_MANAGER_CONNECT で足りる（管理者でないとサービスを開くところで ERROR_ACCESS_DENIED）
	wil::unique_schandle manager{ OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT) };
	THROW_LAST_ERROR_IF_NULL(manager);

	// 2. サービスを止めて削除する。削除には DELETE の権限が要る
	if (wil::unique_schandle service{ OpenServiceW(manager.get(), kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE) }; service) {
		progress(L"サービスを停止しています...");
		StopServiceAndWait(service.get(), 30s);

		// DeleteService は「削除の予約」。このサービスを開いているハンドルがすべて閉じたときに本当に消える。
		// services.msc を開いたままだと残り続け、その間の install は ERROR_SERVICE_MARKED_FOR_DELETE になる
		THROW_IF_WIN32_BOOL_FALSE(DeleteService(service.get()));
		progress(L"サービスを削除しました");
	}
	else {
		const DWORD error = GetLastError();
		THROW_WIN32_IF(error, error != ERROR_SERVICE_DOES_NOT_EXIST);
		progress(L"サービスは登録されていません");
	}

	// 3. キーの割り当て（HKLM\SOFTWARE\DisplayRescue）を消す
	config::DeleteAllSettings();
	progress(L"キーの設定を削除しました");

	// 4. Program Files の exe とフォルダを消す
	const auto directory = InstallDirectory();
	if (DeleteOrScheduleOnReboot(directory / kServiceExe)) {
		// フォルダは exe が消えた後に空になるので、exe → フォルダの順に予約する
		MoveFileExW(directory.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
		progress(std::format(L"実行中のexe自身は今は消せないため、次の再起動で削除されます: {}", directory.wstring()));
	}
	else {
		std::error_code ec;
		fs::remove(directory, ec);	// 空なら消す（ほかのファイルが置かれていたら残す）
	}

	progress(L"アンインストールしました(ログのC:\\ProgramData\\DisplayRescueは調査用に残しています。不要なら削除してください)");
}
