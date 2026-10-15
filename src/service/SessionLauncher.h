#pragma once

#include <windows.h>
#include <wil/resource.h>	//wil::unique_process_information
#include <string_view>

namespace rescue::service {

	// 物理モニタがつながっているセッション（コンソールセッション）の入力デスクトップ（キー入力が届くデスクトップ）に
	// "DisplayRescueService.exe <arguments>"（例 "watch 420"）を起動し、終了は待たずに返す。
	// （下の 3 つとも）program を渡すと、自分ではなく自分と同じフォルダの program（例 "DisplayRescueMascot.exe"）を起動する。
	//   自分と同じフォルダ（インストール先 = Program Files）のものだけ。どこかの exe を SYSTEM で起動する入口にしないため
	//   ロック中・ユーザー不在のログイン画面 → winlogon.exe のアクセストークン（SYSTEM）で winsta0\Winlogon
	//   それ以外（サインイン中で、ロックしていない） → ユーザーのアクセストークンで winsta0\default
	// inheritHandle を渡すとそのハンドル一つだけを子に継承させる（同じ値のまま子でも使える。継承可能なハンドルであること）。
	//   ホットキー監視プロセスに停止イベントを渡すため。一つに限定するのは、サービスが持つほかの継承可能なハンドルを子プロセスへ漏らさないため
	// 起動までの失敗は例外(wil::ResultException)。どのAPIで失敗したかはログに残る。
	// LocalSystemのサービスからしか成功しない(WTSQueryUserToken/CreateProcessAsUserWがSYSTEMの特権を必要とする)
	wil::unique_process_information StartInInputDesktop(std::wstring_view arguments, HANDLE inheritHandle = nullptr, std::wstring_view program = {});

	// コンソールセッションのユーザーアクセストークンを使用し、UIAccess 付きで winsta0\default に起動する。ロック中でも Default に起動する。
	//   ロック画像（LockApp）は Default デスクトップにいるので、その上（ZBID_UIACCESS の層）に出すため（マスコットのロック画像用）。
	//   ユーザーがいなければ ERROR_NO_TOKEN の例外
	wil::unique_process_information StartInUserDesktopWithUiAccess(std::wstring_view arguments, HANDLE inheritHandle = nullptr, std::wstring_view program = {});

	// コンソールセッションの winlogon.exe のアクセストークン（SYSTEM）を使用し、ロック状態に関係なく winsta0\Winlogon に起動する。
	//   パスワード欄・ログイン画面（Winlogon デスクトップ）の上に出すため（マスコットのパスワード欄用）。
	//   Winlogon デスクトップは普段は見えないので、置きっぱなしにしておけば、ロックしたときにそのまま見える
	wil::unique_process_information StartInWinlogonDesktop(std::wstring_view arguments, HANDLE inheritHandle = nullptr, std::wstring_view program = {});

	// コンソールセッションがロック中か（ユーザー不在のログイン画面もロック中になる）。調べられなければ例外
	bool IsConsoleSessionLocked();
}
