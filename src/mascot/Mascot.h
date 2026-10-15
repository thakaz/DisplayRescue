#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>

#include "mascot/PlacementEditor.h"

namespace rescue::mascot {
	// マスコットの調整値。最終的には利用者が GUI で設定する想定なので、奥に埋め込まずにここから渡す
	struct MascotOptions {
		// キャラの中心の位置（画面の左上 0 〜 右下 1。はみ出してもよい）、背の高さ（画面の高さに対する割合）、向き（ラジアン）
		float positionX = 0.87f;
		float positionY = 0.66f;
		float scale = 0.45f;
		float yaw = 0.0f;
		float pitch = 0.0f;
		bool edit = false;				// 調整用の窓にする（設定アプリの「配置を調整」。PlacementEditor で操作し、Enter で設定に保存）
		// 設定アプリの「再生」。このモーション（変換後の .motion）をデスクトップ上で 1 回だけ再生する。
		// 待機モーションがあれば「待機 → このモーション → 待機」の順に切り替え、通常と同じクロスフェードを確認できるようにする。待機モーション自体を指定した場合は、それだけを再生する
		std::optional<std::filesystem::path> previewMotion;
		bool showOnDesktop = false;		// 普段のデスクトップにも出すか。false ならロック中だけ見せる（Winlogon デスクトップでは関係ない）
		bool showMascot = true;			// キャラクターを表示するか。false（またはモデルがない）の場合はキーの案内だけを表示する
		bool showShortcuts = true;		// ロック中に画面切り替えキーの案内を表示するか
		double motionIntervalSeconds = 30.0;	// 1 回の待機の長さ（この間隔ごとに、ほかのモーションを 1 本再生する）
		float fadeSeconds = 0.5f;		// モーションのクロスフェード時間（待機 → 動作）
		float idleFadeSeconds = 1.5f;	// 動作の終了後、待機に戻るまでのクロスフェード時間（どちらも設定アプリで変更できる）
		float blinkInterval = 4.0f;		// まばたきの間隔（秒）
		float appearSeconds = 0.4f;		// 見え始めてから、すっかり出るまでの秒数
	};

	// サービスが置きっぱなしにするマスコット 1 つ。
	// %ProgramData%\DisplayRescue\mascot.model を options の位置と大きさで描き、mascot-motions フォルダ（サブフォルダも）の .motion を
	// ランダムに選んだモーションと待機モーション（mascot-idle.motion）を交互にクロスフェードして再生する（モーションがなければ立ったまま、まばたきだけを行う）。
	// どちらも設定アプリが取り込むときに変換したもの（AssetFile.h）。クリック・キー入力・フォーカスは奪わない。
	// stopEvent: サービスが継承させた「止めて」のイベント（watch と同じ）。立ったら終わる。nullptr（手で起動したとき）なら止めては来ない。
	// ログは rescue.log に [mascot] 付きで書く（書けるのは SYSTEM のとき＝パスワード欄用だけ。ロック画像用はユーザー権限で動くので書けない）。
	// 戻り値は終了コード（0 = 止めてと言われて終わった。モデルが無ければ ERROR_FILE_NOT_FOUND）。
	// options.edit なら、窓は画面いっぱいでクリックとキーを受け、Enter で位置・大きさ・向きを設定に保存して 0、Esc なら ERROR_CANCELLED
	// options.previewMotion なら、再生し終わったら 0（読めなければ ERROR_FILE_NOT_FOUND など）
	int RunMascot(HANDLE stopEvent, const MascotOptions& options = {});
}
