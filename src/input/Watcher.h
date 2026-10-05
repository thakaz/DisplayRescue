#pragma once

#include <windows.h>

namespace rescue::input {
	// 待ち受け役（watch）。
	// 割り当て（config::LoadBindings）をすべて RegisterHotKey し、押されたらその場で SetDisplayConfig する。
	// 届くのは「自分のいるデスクトップ」へのキー入力だけ（Default と Winlogon は別。ロック中は Winlogon にいないと届かない）。
	// 登録した組み合わせ以外のキーは受け取らない（WM_HOTKEY しか来ないので、キーロガーにならない）。
	// stopEvent が立ったら登録を外して 0 で終わる。nullptr なら終わらない（コンソールから手で動かすとき。Ctrl+C で止める）。
	// それ以外の戻り値は失敗。
	// 登録の失敗と切り替えの結果はコンソールと rescue.log の両方に出す。サービスから起動されたときは stdout がどこにもつながっていないため。
	// ログに残るのは SYSTEM で動いているとき（ロック画面・ログイン画面）だけ（ユーザーの身分ではログのフォルダに書けない）
	int RunWatch(HANDLE stopEvent);
}
