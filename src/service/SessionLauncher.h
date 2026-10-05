#pragma once

#include <windows.h>
#include <wil/resource.h>	//wil::unique_process_information
#include <string_view>

namespace rescue::service {

	// 物理モニタがつながっているセッション（コンソールセッション）の入力デスクトップ（キー入力が届くデスクトップ）に
	// "DisplayRescueService.exe <arguments>"（例 "watch 420"）を起動し、終了は待たずに返す。
	//   ロック中・ユーザー不在のログイン画面 → winlogon.exe の身分（SYSTEM）で winsta0\Winlogon
	//   それ以外（サインイン中で、ロックしていない） → ユーザーの身分で winsta0\default
	// inheritHandle を渡すとそのハンドル一つだけを子に継承させる（同じ値のまま子でも使える。継承可能なハンドルであること）。
	//   待ち受け役に「止めて」のイベントを渡すため。一つに絞るのはサービスの他の継承可能なハンドルを子に漏らさないため
	// 起動までの失敗は例外(wil::ResultException)。どのAPIで失敗したかはログに残る。
	// LocalSystemのサービスからしか成功しない(WTSQueryUserToken/CreateProcessAsUserWがSYSTEMの特権を必要とする)
	wil::unique_process_information StartInInputDesktop(std::wstring_view arguments, HANDLE inheritHandle = nullptr);
}
