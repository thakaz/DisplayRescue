#pragma once

// マスコットの設定（HKLM\SOFTWARE\DisplayRescue\Mascot）と、モデル・モーションの置き場所。
// キーの割り当て（config/Settings.*）と同じ形: 置き場所は管理者と SYSTEM しか書けない HKLM。読むのは一般ユーザーでもよい
// （ロック画像用のマスコットはサインイン中のユーザー権限で動作する）

#include <cstdint>
#include <filesystem>
#include <string>

namespace rescue::config {
	struct MascotSettings {
		bool enabled = false;				// マスコットを出すか（既定は出さない）
		bool showOnDesktop = false;			// 普段のデスクトップにも出すか（オフならロック中だけ見せる）
		// ロック中、各モニタに画面切り替えキーの案内を表示するか。マスコットとは別に設定できる。
		// 既定は出す（DisplayRescue の出番＝ほかの画面が見えているときに、何を押せばよいか分かるように）
		bool showShortcuts = true;
		// マスコットを常駐させるか。オフ（既定）はロックしてから読み込む（出るまで数秒かかるが、普段はメモリを使わない）。
		// オンはいつも置いておく（ロックしてすぐ出るが、1 つで数百 MB を 2 つ分使い続ける）
		bool keepResident = false;
		// 配置。利用者から見ると枠は無く、キャラの位置と大きさと向きだけ。
		// 位置と大きさはメインのモニタ全体（タスクバーも含む）に対する割合で持つ。ピクセルで持つと、DisplayRescue が画面の構成を
		// 切り替えて画面が変わったとき、画面の外に出てしまうため。キャラは画面からはみ出してよい（顔だけアップ）ので、位置は 0〜1000 の外も取る
		int positionX = 870;				// キャラの中心の横の位置（画面の左端 0 〜 右端 1000）
		int positionY = 660;				// キャラの中心の縦の位置（画面の上端 0 〜 下端 1000）
		int scalePercent = 45;				// キャラの背の高さ（画面の高さに対する %。数倍にすれば顔だけアップ）
		int yawDegrees = 0;					// 左右の向き（0 = 正面、+ でキャラが自分の左を向く）
		int pitchDegrees = 0;				// 前後の傾き（+ で上から見下ろす）
		int motionIntervalSeconds = 30;		// 待機中にほかのモーションを再生する間隔（1 回の待機時間）
		int fadeMilliseconds = 500;			// モーションを切り替える時間（待機 → 動作、動作 → 動作）のミリ秒
		int idleFadeMilliseconds = 1500;	// 動作の終了後、待機に戻るまでの切り替え時間（ミリ秒）

		bool operator==(const MascotSettings&) const = default;
	};

	// 範囲（読み込み・保存のときに収める）
	inline constexpr int kMinPosition = -1000;
	inline constexpr int kMaxPosition = 2000;
	inline constexpr int kMinScalePercent = 10;
	inline constexpr int kMaxScalePercent = 1000;
	inline constexpr int kMaxYawDegrees = 180;
	inline constexpr int kMaxPitchDegrees = 80;
	inline constexpr int kMinMotionIntervalSeconds = 5;
	inline constexpr int kMaxMotionIntervalSeconds = 3600;
	inline constexpr int kMaxFadeMilliseconds = 5000;

	// 読む。キーや値が無ければ既定値、範囲の外は収める。キーを開けない（アクセス拒否など）ときは例外
	MascotSettings LoadMascotSettings();

	// 書く（範囲は収めてから）。管理者でないとアクセス拒否の例外
	void SaveMascotSettings(const MascotSettings& settings);

	// マスコットが読むのは、設定アプリが取り込むときに変換した形式（mascot/AssetFile.*）だけ。
	// 先頭は印（8 バイト）と版（4 バイト）。版が違えば読まない（設定アプリが元のファイルから変換し直す）
	inline constexpr char kMascotModelMagic[8] = { 'D', 'R', 'M', 'O', 'D', 'E', 'L', 0 };
	inline constexpr char kMascotMotionMagic[8] = { 'D', 'R', 'M', 'O', 'T', 'I', 'O', 'N' };
	// 版 2: モーションに表情（VRMC_vrm_animation の expressions）を足した
	inline constexpr std::uint32_t kMascotAssetVersion = 2;

	// モデル（%ProgramData%\DisplayRescue\mascot.model）とモーションのフォルダ（%ProgramData%\DisplayRescue\mascot-motions の *.motion）。
	// SYSTEM（パスワード欄用のマスコット）が読むので、管理者しか書けないフォルダ（install が SecureLogDirectory で作る）に置く
	std::filesystem::path MascotModelPath();
	std::filesystem::path MascotMotionDirectory();

	// 待機モーション（%ProgramData%\DisplayRescue\mascot-idle.motion）。あれば待機あり、無ければ無し。
	// モーションのフォルダの外に置く（乱数で選ぶモーションと混ざらないように）
	std::filesystem::path MascotIdleMotionPath();

	// 取り込んだ元のファイル（.vrm / .vrma）の置き場所（%ProgramData%\DisplayRescue\mascot-sources）。
	// SYSTEM のマスコットは読まない。形式の版が変わったとき、設定アプリがここから変換し直す
	std::filesystem::path MascotSourceDirectory();
}
