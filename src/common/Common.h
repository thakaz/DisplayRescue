#pragma once

#include <Windows.h>

#include <optional>
#include <string>
#include <string_view>

namespace rescue {
	//コマンドラインで受け取った、継承したハンドルの値（10 進。例 "420"）を HANDLE にする。数でない・0 なら nullopt。
	// 継承したハンドルは子プロセスでも親と同じ値で使用できる（サービスからホットキー監視プロセスとマスコットへ停止イベントを渡すために使う）
	std::optional<HANDLE> ParseInheritedHandle(const wchar_t* text);

	//そのセッションがロック中か（WTSSessionInfoEx の SessionFlags）。ユーザー不在のログイン画面もロック中になる。
	//sessionId に WTS_CURRENT_SESSION を渡すと自分のセッション。調べられなければ例外
	bool IsSessionLocked(DWORD sessionId);

	//大文字小文字を無視して比較する
	bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b);

	//Win32 エラーコードを名前にする（例 "ERROR_ACCESS_DENIED"）。
	//表に無いコードは 16 進と Windows の説明文（例 "0x00000581 ホット キーは既に登録されています。"）
	std::wstring DescribeError(LONG rc);

	//HRESULTを名前にする。Win32エラーから作られたものは元のWin32エラー名、それ以外は16進。
	std::wstring DescribeHresult(HRESULT hr);

	//HRESULT を終了コードにする。Win32 エラーから作られたものは元の Win32 エラーコード、それ以外は HRESULT の値そのまま
	int ExitCodeFromHresult(HRESULT hr);
}
