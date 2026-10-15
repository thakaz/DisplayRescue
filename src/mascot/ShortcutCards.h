#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <string>
#include <vector>

#include "mascot/Monitors.h"

namespace rescue::mascot {
	// 案内の 1 行（"Ctrl+Alt+F8" と "拡張"）
	struct ShortcutLine {
		std::wstring keys;
		std::wstring action;
	};

	// ロック画面で、画面切り替えキーの一覧を各モニタに表示する。
	// 拡張のときにサブの画面が見えている＝ DisplayRescue の出番なので、どの画面を見ていてもキーが分かるようにする。
	// 案内は 1 モニタにつき 1 つの小さいウィンドウ（左上）。表示内容は変わらないため、D3D ではなく、1 回だけ Direct2D で描いた画素を
	// UpdateLayeredWindow で窓に渡す（画素ごとの透明度つき。描き直すのは構成が変わったときだけ）。
	// マスコットと同じく、最前面・クリック素通し・フォーカスを取らない。同じスレッド（マスコットのループ）から使う
	class ShortcutCards {
	public:
		explicit ShortcutCards(std::vector<ShortcutLine> lines);
		ShortcutCards(const ShortcutCards&) = delete;
		ShortcutCards& operator=(const ShortcutCards&) = delete;
		// 移すのはよい（窓は自分を指していないので、持ち主が変わっても困らない）。optional に入れて関数から返すため
		ShortcutCards(ShortcutCards&&) = default;
		ShortcutCards& operator=(ShortcutCards&&) = default;

		// モニタごとに案内を作り直す（以前の案内は削除する）。表示状態と透明度は維持する
		void Place(const std::vector<MonitorArea>& monitors);
		// 見せる・隠す（変わったときだけ ShowWindow）
		void SetVisible(bool visible);
		// 透明度（0〜1）。マスコットと一緒にふわっと出す。変わったときだけ窓に渡し直す
		void SetOpacity(float opacity);
		// 最前面にし直す（ロック画面の部品に負けないように。マスコットの kTimerRaise と同じ）
		void Raise() const;

	private:
		struct Card {
			wil::unique_hwnd hwnd;
			wil::unique_hbitmap bitmap;	// 描いた画素（32 ビット、透明度は掛け済み）
			POINT position{};			// 窓の左上（画面の座標）
			SIZE size{};
		};

		Card MakeCard(const MonitorArea& monitor);
		void Update(const Card& card) const;	// 画素と今の透明度を窓に渡す

		std::vector<ShortcutLine> m_lines;
		wil::com_ptr<ID2D1Factory> m_d2d;
		wil::com_ptr<IDWriteFactory> m_dwrite;
		wil::com_ptr<ID2D1DCRenderTarget> m_target;	// メモリの DC（の中のビットマップ）に描く Direct2D の口
		std::vector<Card> m_cards;
		bool m_visible = false;
		BYTE m_alpha = 0;
	};
}
