#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

#include "common/FileVersion.h"

namespace rescue::service {
	// 途中経過を 1 行ずつ受け取る関数（CLI はコンソールに、GUI はメッセージ欄に出す）
	using Progress = std::function<void(std::wstring_view)>;

	// インストール先のフォルダ（C:\Program Files\DisplayRescue）
	std::filesystem::path InstallDirectory();

	// インストール済みの版（インストール先の DisplayRescueService.exe のバージョン情報）。インストールされていなければ nullopt
	std::optional<FileVersion> InstalledVersion();

	// インストールする。管理者で実行する。すでにあれば止めてから上書きし、設定を更新する（再インストール・更新）。
	//   1. ログのフォルダを管理者と SYSTEM だけが書ける状態にする
	//   2. 動いているサービスを止める
	//   3. 今動いている exe と同じフォルダ（配布した zip を展開した場所）にある DisplayRescueService.exe をインストール先にコピーする。
	//      インストール先に置くのはサービスが動かすこの exe だけ（SYSTEM で動くので、一般ユーザーが書き換えられない場所に置く）。
	//      設定画面は展開したフォルダから開く（ポータブル版と同じ）ので、コピーしない
	//   4. 自動起動・LocalSystem のサービスとして登録し、説明文と回復オプションを設定して起動する
	// 失敗は例外（wil::ResultException。管理者でなければ ERROR_ACCESS_DENIED）。どの API で失敗したかは WIL がログに残す
	void Install(const Progress& progress);

	// アンインストールする。管理者で実行する。
	// サービスを止めて削除し、キーの設定（HKLM\SOFTWARE\DisplayRescue）とインストール先の exe を消す。
	// インストール先の exe 自身から実行したときは今は消せないので、次の再起動で消えるよう予約する。
	// ログ（C:\ProgramData\DisplayRescue）は調査用に残す。失敗は例外
	void Uninstall(const Progress& progress);
}
