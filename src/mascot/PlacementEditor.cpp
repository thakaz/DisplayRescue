#include "mascot/PlacementEditor.h"

#include <windowsx.h>	// GET_X_LPARAM / GET_Y_LPARAM

#include <algorithm>
#include <cmath>

namespace rescue::mascot {
	std::optional<LRESULT> PlacementEditor::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
		switch (message) {
		// 1. ボタンを押したら、どちらのドラッグかを覚えてマウスを捕まえる（窓の外に出ても WM_MOUSEMOVE が届く）
		case WM_LBUTTONDOWN:
		case WM_RBUTTONDOWN:
			m_drag = message == WM_LBUTTONDOWN ? Drag::Move : Drag::Rotate;
			m_lastMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			SetCapture(hwnd);
			return 0;

		// 2. 離したらマウスを放す。放すと WM_CAPTURECHANGED が来るので、ドラッグの終わりはそこでまとめる
		case WM_LBUTTONUP:
		case WM_RBUTTONUP:
			ReleaseCapture();
			return 0;
		case WM_CAPTURECHANGED:	// 放した・ほかに取られた（Alt+Tab など）
			m_drag = Drag::None;
			return 0;

		// 3. ドラッグ中なら、前の位置からどれだけ動いたかで、動かすか回す
		case WM_MOUSEMOVE: {
			if (m_drag == Drag::None) return 0;
			const POINT now{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			const float dx = static_cast<float>(now.x - m_lastMouse.x);
			const float dy = static_cast<float>(now.y - m_lastMouse.y);
			m_lastMouse = now;
			if (m_drag == Drag::Move) {
				m_placement.centerX += dx;
				m_placement.centerY += dy;
			}
			else {
				m_placement.yaw -= dx * kRotateSpeed;
				m_placement.pitch = std::clamp(m_placement.pitch - dy * kRotateSpeed, -kMaxPitch, kMaxPitch);
			}
			return 0;
		}

		// 4. ホイールで、マウスの所を中心に拡大・縮小する（中心 = マウス + (中心 - マウス) × 割合）
		case WM_MOUSEWHEEL: {
			const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
			const float scale = std::clamp(m_placement.scale * std::pow(kWheelStep, notches), kMinScale, kMaxScale);
			const float ratio = scale / m_placement.scale;
			// WM_MOUSEWHEEL の位置だけは画面の座標なので、窓の座標に直す
			POINT cursor{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			ScreenToClient(hwnd, &cursor);
			m_placement.centerX = cursor.x + (m_placement.centerX - cursor.x) * ratio;
			m_placement.centerY = cursor.y + (m_placement.centerY - cursor.y) * ratio;
			m_placement.scale = scale;
			return 0;
		}

		// 5. キー。R で向きを戻す、Enter で決める、Esc でやめる
		case WM_KEYDOWN:
			if (wParam == 'R') {
				m_placement.yaw = 0.0f;
				m_placement.pitch = 0.0f;
				return 0;
			}
			if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
				m_finished = true;
				m_accepted = wParam == VK_RETURN;
				return 0;
			}
			break;
		}
		return std::nullopt;	// 扱わなかった（呼ぶ側の WindowProc が DefWindowProcW に回す）
	}
}
