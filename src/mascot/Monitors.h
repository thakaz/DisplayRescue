#pragma once

#include <windows.h>

#include <vector>

namespace rescue::mascot {
	// モニタ 1 つの場所。座標はどれも画面の座標（仮想スクリーン。メインの左上が 0,0）
	struct MonitorArea {
		HMONITOR monitor = nullptr;	// DPI を聞くため。比べるときは見ない（構成が変わると作り直されることがある）
		RECT bounds{};				// モニタ全体
		RECT work{};				// 作業領域（タスクバーを除いた部分。ロック画像（Default デスクトップ）ではタスクバーが見えていなくても除かれる）
		bool primary = false;		// メインのモニタか

		// RECT には == が無いので自分で書く。HMONITOR は比べない
		bool operator==(const MonitorArea& other) const;
	};

	// 今つながっている（デスクトップに出ている）モニタを、左上から順に。複製のときは重なったモニタが 1 つとして返る
	// EnumDisplayMonitors は「モニタごとに呼んでほしい関数」と「その関数に渡す値（LPARAM）」を受け取る。値に vector のアドレスを渡して詰めてもらう
	std::vector<MonitorArea> EnumerateMonitors();

	// メインのモニタ全体。見つからなければ（まず無いが）原点からメインの画面の大きさ。
	// マスコットは作業領域ではなくこちらを使う。ロック中しか出さないので、タスクバーをよけると下に空白ができて切れて見えるため
	RECT PrimaryMonitorArea(const std::vector<MonitorArea>& monitors);
}
