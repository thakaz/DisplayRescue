// 流れ:
//   Place → モニタごとに MakeCard（(1) 大きさを測る → (2) 画素の紙を作る → (3) Direct2D で描く → (4) 窓を作る）→ Update（UpdateLayeredWindow）
//   SetOpacity / SetVisible はマスコットのループから毎フレーム呼ばれるが、値が変わったときしか窓に触らない。
// 窓は Mascot.cpp と同じく最前面・クリック素通し。中身は D3D ではなく、画素ごとの透明度を持つ窓（UpdateLayeredWindow）に DirectWrite の文字を描く
#include "mascot/ShortcutCards.h"

#include <ShellScalingApi.h>	// GetDpiForMonitor
#include <wil/result.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "shcore.lib")

namespace {
	constexpr wchar_t kClassName[] = L"DisplayRescueShortcutCard";
	constexpr wchar_t kFontFamily[] = L"Yu Gothic UI";
	constexpr wchar_t kTitle[] = L"画面の切り替え";

	// 大きさは 96 DPI（拡大 100%）のときのピクセル。モニタの拡大率を掛けて使う
	constexpr float kTitleSize = 13.0f;
	constexpr float kLineSize = 17.0f;
	constexpr float kPadding = 14.0f;
	constexpr float kTitleGap = 6.0f;		// 見出しと行の間
	constexpr float kColumnGap = 18.0f;		// キーと操作の名前の間
	constexpr float kRadius = 10.0f;		// 角の丸み
	constexpr float kMargin = 24.0f;		// モニタの左上からの距離

	LRESULT CALLBACK CardProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
		switch (message) {
		case WM_NCHITTEST:		return HTTRANSPARENT;	// マウスは下の窓に回す
		case WM_MOUSEACTIVATE:	return MA_NOACTIVATE;	// クリックされても前面にならない
		}
		return DefWindowProcW(hwnd, message, wParam, lParam);
	}

	// モニタの拡大率（1.0 = 100%）。プロセスはモニタごとの DPI を扱う宣言をしているので、モニタごとに違う値が返る
	float ScaleOf(HMONITOR monitor) {
		UINT dpiX = 96, dpiY = 96;
		if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) return 1.0f;
		return dpiX / 96.0f;
	}

	// 改行でつないだ 1 つの文字列（キーの列・操作の名前の列を、それぞれ 1 つの文字の並びとして測って描く）
	template <typename Projection>
	std::wstring JoinLines(const std::vector<rescue::mascot::ShortcutLine>& lines, Projection project) {
		std::wstring text;
		for (const auto& line : lines) {
			if (!text.empty()) text += L'\n';
			text += project(line);
		}
		return text;
	}
}

namespace rescue::mascot {
	ShortcutCards::ShortcutCards(std::vector<ShortcutLine> lines) : m_lines{ std::move(lines) } {
		THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2d.put()));
		THROW_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwrite.put())));

		// DC に描く口。UpdateLayeredWindow に渡すので、形式は BGRA で透明度は掛け済み（PREMULTIPLIED）。
		// DPI は 96 にして「1 単位 = 1 ピクセル」にし、拡大率は自分で掛ける（モニタごとに違うため）
		const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
			D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
		THROW_IF_FAILED(m_d2d->CreateDCRenderTarget(&properties, m_target.put()));

		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof(windowClass);
		windowClass.lpfnWndProc = CardProc;
		windowClass.hInstance = GetModuleHandleW(nullptr);
		windowClass.lpszClassName = kClassName;
		if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) THROW_LAST_ERROR();
	}

	void ShortcutCards::Place(const std::vector<MonitorArea>& monitors) {
		m_cards.clear();	// 前の窓は unique_hwnd が DestroyWindow する
		for (const MonitorArea& monitor : monitors) {
			m_cards.push_back(MakeCard(monitor));
			Update(m_cards.back());
			if (m_visible) ShowWindow(m_cards.back().hwnd.get(), SW_SHOWNOACTIVATE);
		}
	}

	ShortcutCards::Card ShortcutCards::MakeCard(const MonitorArea& monitor) {
		const float scale = ScaleOf(monitor.monitor);

		// 1. 大きさを測る。見出し・キーの列・名前の列を、それぞれ文字の並び（IDWriteTextLayout）にして GetMetrics で幅と高さを聞く
		const auto makeLayout = [&](const std::wstring& text, float size, DWRITE_FONT_WEIGHT weight) {
			wil::com_ptr<IDWriteTextFormat> format;
			THROW_IF_FAILED(m_dwrite->CreateTextFormat(kFontFamily, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
				size * scale, L"ja-jp", format.put()));
			wil::com_ptr<IDWriteTextLayout> layout;
			THROW_IF_FAILED(m_dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.get(), 10000.0f, 10000.0f, layout.put()));
			DWRITE_TEXT_METRICS metrics{};
			THROW_IF_FAILED(layout->GetMetrics(&metrics));
			return std::pair{ layout, metrics };
		};
		const auto [title, titleSize] = makeLayout(kTitle, kTitleSize, DWRITE_FONT_WEIGHT_NORMAL);
		const auto [keys, keysSize] = makeLayout(JoinLines(m_lines, [](const ShortcutLine& l) { return l.keys; }), kLineSize, DWRITE_FONT_WEIGHT_SEMI_BOLD);
		const auto [actions, actionsSize] = makeLayout(JoinLines(m_lines, [](const ShortcutLine& l) { return l.action; }), kLineSize, DWRITE_FONT_WEIGHT_NORMAL);

		const float padding = kPadding * scale;
		const float rowsTop = padding + titleSize.height + kTitleGap * scale;
		const float actionsLeft = padding + keysSize.widthIncludingTrailingWhitespace + kColumnGap * scale;
		const float width = (std::max)(padding + titleSize.widthIncludingTrailingWhitespace, actionsLeft + actionsSize.widthIncludingTrailingWhitespace) + padding;
		const float height = rowsTop + (std::max)(keysSize.height, actionsSize.height) + padding;

		Card card;
		card.size = { static_cast<LONG>(std::ceil(width)), static_cast<LONG>(std::ceil(height)) };
		card.position = { monitor.work.left + static_cast<LONG>(kMargin * scale), monitor.work.top + static_cast<LONG>(kMargin * scale) };

		// 2. 画素の紙。32 ビットの DIB（上から下の並びにするため高さは負）。中身は Direct2D が書く
		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(info.bmiHeader);
		info.bmiHeader.biWidth = card.size.cx;
		info.bmiHeader.biHeight = -card.size.cy;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		void* bits = nullptr;
		card.bitmap.reset(CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0));
		THROW_LAST_ERROR_IF_NULL(card.bitmap.get());

		// 3. 描く。メモリの DC に紙を差し込み（SelectObject）、その DC を Direct2D の口に結びつける。差し込んだ紙は最後に元に戻す
		const wil::unique_hdc dc{ CreateCompatibleDC(nullptr) };
		THROW_LAST_ERROR_IF_NULL(dc.get());
		const HGDIOBJ previous = SelectObject(dc.get(), card.bitmap.get());
		const auto restore = wil::scope_exit([&] { SelectObject(dc.get(), previous); });
		const RECT bounds{ 0, 0, card.size.cx, card.size.cy };
		THROW_IF_FAILED(m_target->BindDC(dc.get(), &bounds));

		wil::com_ptr<ID2D1SolidColorBrush> background, dim, bright;
		THROW_IF_FAILED(m_target->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.06f, 0.09f, 0.78f), background.put()));
		THROW_IF_FAILED(m_target->CreateSolidColorBrush(D2D1::ColorF(0.75f, 0.77f, 0.82f), dim.put()));
		THROW_IF_FAILED(m_target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), bright.put()));

		m_target->BeginDraw();
		m_target->Clear(D2D1::ColorF(0, 0, 0, 0));
		m_target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, width, height), kRadius * scale, kRadius * scale), background.get());
		m_target->DrawTextLayout(D2D1::Point2F(padding, padding), title.get(), dim.get());
		m_target->DrawTextLayout(D2D1::Point2F(padding, rowsTop), keys.get(), bright.get());
		m_target->DrawTextLayout(D2D1::Point2F(actionsLeft, rowsTop), actions.get(), bright.get());
		THROW_IF_FAILED(m_target->EndDraw());

		// 4. 窓。マスコットと同じ: 最前面・タスクバーに出さない・クリック素通し・フォーカスを取らない。
		// WS_EX_LAYERED の窓は、UpdateLayeredWindow で画素を渡すまで何も映らない
		card.hwnd.reset(CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
			kClassName, L"DisplayRescue shortcuts", WS_POPUP,
			card.position.x, card.position.y, card.size.cx, card.size.cy, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr));
		THROW_LAST_ERROR_IF_NULL(card.hwnd.get());
		return card;
	}

	void ShortcutCards::Update(const Card& card) const {
		const wil::unique_hdc dc{ CreateCompatibleDC(nullptr) };
		THROW_LAST_ERROR_IF_NULL(dc.get());
		const HGDIOBJ previous = SelectObject(dc.get(), card.bitmap.get());
		const auto restore = wil::scope_exit([&] { SelectObject(dc.get(), previous); });

		// 画素の透明度（AC_SRC_ALPHA）に、窓全体の透明度（SourceConstantAlpha）を掛けて合成してもらう
		BLENDFUNCTION blend{ AC_SRC_OVER, 0, m_alpha, AC_SRC_ALPHA };
		POINT position = card.position;
		SIZE size = card.size;
		POINT origin{ 0, 0 };
		THROW_IF_WIN32_BOOL_FALSE(UpdateLayeredWindow(card.hwnd.get(), nullptr, &position, &size, dc.get(), &origin, 0, &blend, ULW_ALPHA));
	}

	void ShortcutCards::SetVisible(bool visible) {
		if (visible == m_visible) return;
		m_visible = visible;
		for (const Card& card : m_cards) ShowWindow(card.hwnd.get(), visible ? SW_SHOWNOACTIVATE : SW_HIDE);
	}

	void ShortcutCards::SetOpacity(float opacity) {
		const BYTE alpha = static_cast<BYTE>(std::lround(std::clamp(opacity, 0.0f, 1.0f) * 255.0f));
		if (alpha == m_alpha) return;
		m_alpha = alpha;
		for (const Card& card : m_cards) Update(card);
	}

	void ShortcutCards::Raise() const {
		if (!m_visible) return;	// 隠しているときは触らない（SWP_SHOWWINDOW を付けなくても、順番だけ変える）
		for (const Card& card : m_cards) SetWindowPos(card.hwnd.get(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
}
