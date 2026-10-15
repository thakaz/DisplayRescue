#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>	// IDXGISwapChain1 / IDXGIFactory2（CreateSwapChainForComposition）
#include <dcomp.h>		// DirectComposition
#include <wil/com.h>	// wil::com_ptr

#include <array>
#include <string>

namespace rescue::mascot {
	// 背景が透明なウィンドウに D3D11 で描くための「画用紙」。
	//   デバイス・スワップチェーン・描く先と奥行き・DirectComposition を、何を描くかと切り離して持つ。
	//   描くもの（Model など）はこれの Device / Context を借りて描く
	// ウィンドウは WS_EX_NOREDIRECTIONBITMAP 付きで作る。
	// 描く先はスワップチェーンではなく、アンチエイリアス（MSAA: 1 ピクセルを 4 点で塗る）用の別の紙。
	//   色は sRGB として書き込む（シェーダーは明るさを「光の量」のまま計算し、書き込むときに画面用に変換される）。
	//   End で 4 点を 1 色に混ぜて（resolve）スワップチェーンに写してから Present する
	class Canvas {
	public:
		// width / height: 描く大きさ（ピクセル。ウィンドウと同じ）。失敗は例外
		Canvas(HWND hwnd, UINT width, UINT height);

		// 1 フレームの始め。描く先と奥行きを付けて、clear の色（既定は透明）と奥行き最大で塗りつぶし、ビューポートを決める。
		// clear は R, G, B, 透明度。色には透明度を掛けておく（PREMULTIPLIED）
		void Begin(const std::array<float, 4>& clear = { 0, 0, 0, 0 });

		// 1 フレームの終わり。MSAA の紙をスワップチェーンに写して（resolve）、画面に出す（垂直同期を 1 回待つ）。戻り値は Present の結果
		HRESULT End();

		ID3D11Device* Device() const noexcept { return m_device.get(); }
		ID3D11DeviceContext* Context() const noexcept { return m_context.get(); }
		const std::wstring& Adapter() const noexcept { return m_adapter; }	// どの GPU か（ログ用）

		// 大きさを変える（ウィンドウの大きさを変えたあとに呼ぶ）。スワップチェーンの紙を作り直し、MSAA の紙と奥行きも同じ大きさで作り直す
		void Resize(UINT width, UINT height);

		// 画用紙全体の不透明度（0 = 見えない、1 = そのまま）。DirectComposition が貼るときに掛けるので、描いた中身は変えない。
		// 変わったときだけ Commit する
		void SetOpacity(float opacity);

	private:
		// 描く先（スワップチェーンの紙・MSAA の紙・描く口・奥行き）を今の大きさで作る。コンストラクタと Resize が使う
		void CreateTargets();

		UINT m_width;
		UINT m_height;
		std::wstring m_adapter;
		UINT m_sampleCount = 1;

		wil::com_ptr<ID3D11Device> m_device;
		wil::com_ptr<ID3D11DeviceContext> m_context;
		wil::com_ptr<IDXGISwapChain1> m_swapChain;
		wil::com_ptr<ID3D11Texture2D> m_backBuffer;		// スワップチェーンの紙（resolve の写し先）
		wil::com_ptr<ID3D11Texture2D> m_msaaTexture;	// MSAA の紙（実際に描く先）
		wil::com_ptr<ID3D11RenderTargetView> m_renderTarget;	// m_msaaTexture を sRGB として見る口
		wil::com_ptr<ID3D11DepthStencilView> m_depth;		// 奥行きも同じ点の数

		wil::com_ptr<IDCompositionDevice> m_composition;
		wil::com_ptr<IDCompositionTarget> m_target;
		wil::com_ptr<IDCompositionVisual> m_visual;
		wil::com_ptr<IDCompositionEffectGroup> m_effect;	// 不透明度を掛ける
		float m_opacity = 1.0f;
	};
}
