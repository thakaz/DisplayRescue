#pragma once

#include <string_view>

namespace rescue {
	//%ProgramData%\DisplayRescue\rescue.logに１行追記する。
	void Log(std::wstring_view message) noexcept;

	//WILが検出した失敗を全て↑のLogに流すようにする。
	void RouteWilFailuresToLog();

	//ログのフォルダ(%ProgramData%\DisplayRescue)を、管理者と SYSTEM だけが書ける状態にする(無ければ作る)。install から管理者で呼ぶ。
	//一般ユーザーが先に作っていたフォルダは使わず、名前を変えて脇に退ける。失敗は例外
	void SecureLogDirectory();
}