#pragma once

#include <windows.h>

#include <optional>

namespace rescue::mascot {
	// キャラの置き方。位置は窓の中のピクセル（調整中の窓は画面いっぱいなので、画面の左上が 0, 0）
	struct Placement {
		float centerX = 0.0f;	// キャラの中心が窓のどこに来るか（ピクセル）
		float centerY = 0.0f;
		float scale = 0.45f;	// キャラの背の高さ（画面の高さに対する割合。1 を超えると画面からはみ出す）
		float yaw = 0.0f;		// 左右の向き（ラジアン。0 = 正面）
		float pitch = 0.0f;		// 前後の傾き（ラジアン。+ で上から見下ろす）

		bool operator==(const Placement&) const = default;
	};

	// 調整用のマスコットの窓（画面いっぱい）の操作。
	// WindowProc が受け取ったメッセージを HandleMessage に回し、処理したら（戻り値があれば）それを返す。
	//   左ドラッグ   → キャラを動かす（centerX / centerY）
	//   右ドラッグ   → キャラを回す（yaw / pitch）
	//   ホイール     → マウスのある所を中心に拡大縮小（scale と、中心も動かす）
	//   R            → 回転を元に戻す
	//   Enter / Esc  → 保存して終わる / やめる
	// 描き直しと保存は RunMascot が Current と IsFinished を見て行う。ここは値を変えるだけ
	class PlacementEditor {
	public:
		explicit PlacementEditor(Placement initial) : m_placement{ initial } {}

		std::optional<LRESULT> HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

		const Placement& Current() const noexcept { return m_placement; }
		bool IsFinished() const noexcept { return m_finished; }
		bool IsAccepted() const noexcept { return m_accepted; }	// Enter で終わったか（Esc なら false）

		static constexpr float kMinScale = 0.1f;
		static constexpr float kMaxScale = 10.0f;
		static constexpr float kMaxPitch = 1.4f;		// 前後の傾きの上限（ラジアン。約 80 度。真上・真下まで行かないように）
		static constexpr float kWheelStep = 1.1f;		// ホイール 1 目盛りで何倍にするか
		static constexpr float kRotateSpeed = 0.01f;	// 右ドラッグ 1 ピクセルで何ラジアン回すか

	private:
		enum class Drag { None, Move, Rotate };	// 今どのボタンでドラッグしているか

		Placement m_placement;
		Drag m_drag = Drag::None;
		POINT m_lastMouse{};		// ドラッグ中、前のマウスの位置（窓の中の座標）
		bool m_finished = false;
		bool m_accepted = false;
	};
}
