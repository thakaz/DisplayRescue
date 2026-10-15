// 背景が透明なウィンドウに D3D11 で描くための画用紙（流れは Canvas.h）。
// (1)デバイス (2)スワップチェーン (3)描く先と奥行き（MSAA の紙と sRGB の口） (4)DirectComposition でウィンドウに貼る
#include "mascot/Canvas.h"

#include <wil/result.h>

#include <algorithm>
#include <array>
#include <format>

#include "common/Common.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

namespace {
	// MSAA の点の数。4 は D3D11 のほとんどの GPU（と WARP）が対応している
	constexpr UINT kSampleCount = 4;

	// スワップチェーンの形式。DirectComposition に貼るスワップチェーンは _SRGB の形式にできないので、
	// 紙は「型なし」（TYPELESS）で作り、描く口だけ _SRGB、写すときは _UNORM として扱う
	constexpr DXGI_FORMAT kTypeless = DXGI_FORMAT_B8G8R8A8_TYPELESS;
	constexpr DXGI_FORMAT kSrgb = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	constexpr DXGI_FORMAT kUnorm = DXGI_FORMAT_B8G8R8A8_UNORM;
}

namespace rescue::mascot {

	Canvas::Canvas(HWND hwnd, UINT width, UINT height) : m_width(width), m_height(height) {
		// 1. デバイス。GPU がだめなら WARP（CPU で描く）
		constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
		const HRESULT hardware = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
			nullptr, 0, D3D11_SDK_VERSION, m_device.put(), nullptr, m_context.put());
		if (FAILED(hardware)) {
			THROW_IF_FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
				nullptr, 0, D3D11_SDK_VERSION, m_device.put(), nullptr, m_context.put()));
		}

		const auto dxgiDevice = m_device.query<IDXGIDevice>();
		wil::com_ptr<IDXGIAdapter> adapter;
		THROW_IF_FAILED(dxgiDevice->GetAdapter(adapter.put()));
		DXGI_ADAPTER_DESC adapterDesc{};
		THROW_IF_FAILED(adapter->GetDesc(&adapterDesc));
		m_adapter = FAILED(hardware)
			? std::format(L"{} (GPUは使えなかった: {})", adapterDesc.Description, rescue::DescribeHresult(hardware))
			: std::wstring{ adapterDesc.Description };

		// 2. スワップチェーン。DirectComposition に貼るので CreateSwapChainForComposition、背景の透明は PREMULTIPLIED
		wil::com_ptr<IDXGIFactory2> factory;
		THROW_IF_FAILED(adapter->GetParent(IID_PPV_ARGS(factory.put())));
		DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
		swapChainDesc.Width = width;
		swapChainDesc.Height = height;
		swapChainDesc.Format = kUnorm;
		swapChainDesc.SampleDesc.Count = 1;
		swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapChainDesc.BufferCount = 2;
		swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
		swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
		THROW_IF_FAILED(factory->CreateSwapChainForComposition(m_device.get(), &swapChainDesc, nullptr, m_swapChain.put()));

		// その GPU が 4 点に対応しているか（品質の段階が 0 なら非対応）。非対応なら 1 点（アンチエイリアスなし）
		UINT quality = 0;
		THROW_IF_FAILED(m_device->CheckMultisampleQualityLevels(kSrgb, kSampleCount, &quality));
		m_sampleCount = quality > 0 ? kSampleCount : 1;

		// 3. 描く先と奥行き（Resize でも作り直すので CreateTargets に切り出した）
		CreateTargets();

		// 4. DirectComposition でウィンドウに貼る
		THROW_IF_FAILED(DCompositionCreateDevice(dxgiDevice.get(), IID_PPV_ARGS(m_composition.put())));
		THROW_IF_FAILED(m_composition->CreateTargetForHwnd(hwnd, TRUE, m_target.put()));
		THROW_IF_FAILED(m_composition->CreateVisual(m_visual.put()));
		THROW_IF_FAILED(m_visual->SetContent(m_swapChain.get()));
		// 貼り方に「効果」をつける口。不透明度はここで変える（SetOpacity）
		THROW_IF_FAILED(m_composition->CreateEffectGroup(m_effect.put()));
		THROW_IF_FAILED(m_visual->SetEffect(m_effect.get()));
		THROW_IF_FAILED(m_target->SetRoot(m_visual.get()));
		THROW_IF_FAILED(m_composition->Commit());
	}

	// 描く先と奥行き。スワップチェーンの紙に直接は描かず、MSAA の紙を別に作ってそこに描く。
	// スワップチェーンの紙は End で写すときだけ使う（FLIP のスワップチェーンでも、D3D11 は 0 番が常に「次に出す紙」）
	void Canvas::CreateTargets() {
		THROW_IF_FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(m_backBuffer.put())));

		D3D11_TEXTURE2D_DESC msaaDesc{};
		msaaDesc.Width = m_width;
		msaaDesc.Height = m_height;
		msaaDesc.MipLevels = 1;
		msaaDesc.ArraySize = 1;
		msaaDesc.Format = kTypeless;
		msaaDesc.SampleDesc.Count = m_sampleCount;
		msaaDesc.Usage = D3D11_USAGE_DEFAULT;
		msaaDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
		THROW_IF_FAILED(m_device->CreateTexture2D(&msaaDesc, nullptr, m_msaaTexture.put()));

		// 描く口は sRGB。シェーダーが返した「光の量」（リニア）が、書き込むときに画面用の値（sRGB）に変換される
		D3D11_RENDER_TARGET_VIEW_DESC targetDesc{};
		targetDesc.Format = kSrgb;
		targetDesc.ViewDimension = m_sampleCount > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
		THROW_IF_FAILED(m_device->CreateRenderTargetView(m_msaaTexture.get(), &targetDesc, m_renderTarget.put()));

		D3D11_TEXTURE2D_DESC depthDesc{};
		depthDesc.Width = m_width;
		depthDesc.Height = m_height;
		depthDesc.MipLevels = 1;
		depthDesc.ArraySize = 1;
		depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		depthDesc.SampleDesc.Count = m_sampleCount;	// 描く先と同じ点の数にする（違うと描けない）
		depthDesc.Usage = D3D11_USAGE_DEFAULT;
		depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		wil::com_ptr<ID3D11Texture2D> depthTexture;
		THROW_IF_FAILED(m_device->CreateTexture2D(&depthDesc, nullptr, depthTexture.put()));
		THROW_IF_FAILED(m_device->CreateDepthStencilView(depthTexture.get(), nullptr, m_depth.put()));
	}

	// スワップチェーンの紙を握っている参照（m_backBuffer と、描く先として付けたもの）をすべて手放してからでないと、
	// ResizeBuffers は失敗する（DXGI_ERROR_INVALID_CALL）。だから先に描く先を外し、作ったものを捨ててから大きさを変える
	void Canvas::Resize(UINT width, UINT height) {
		width = (std::max)(width, 1u);
		height = (std::max)(height, 1u);
		if (width == m_width && height == m_height) return;

		m_context->OMSetRenderTargets(0, nullptr, nullptr);
		m_renderTarget.reset();
		m_depth.reset();
		m_msaaTexture.reset();
		m_backBuffer.reset();
		m_context->Flush();

		// 0 と UNKNOWN は「数と形式は今のまま」
		THROW_IF_FAILED(m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0));
		m_width = width;
		m_height = height;
		CreateTargets();
	}

	void Canvas::SetOpacity(float opacity) {
		opacity = std::clamp(opacity, 0.0f, 1.0f);
		if (opacity == m_opacity) return;
		m_opacity = opacity;
		THROW_IF_FAILED(m_effect->SetOpacity(opacity));
		THROW_IF_FAILED(m_composition->Commit());
	}

	void Canvas::Begin(const std::array<float, 4>& clear) {
		ID3D11RenderTargetView* const renderTarget = m_renderTarget.get();
		m_context->OMSetRenderTargets(1, &renderTarget, m_depth.get());
		m_context->ClearRenderTargetView(renderTarget, clear.data());
		m_context->ClearDepthStencilView(m_depth.get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

		const D3D11_VIEWPORT viewport{ 0, 0, static_cast<float>(m_width), static_cast<float>(m_height), 0, 1 };
		m_context->RSSetViewports(1, &viewport);
	}

	HRESULT Canvas::End() {
		// MSAA の紙をスワップチェーンの紙に写す。4 点は 1 色に混ぜる（resolve）。
		// 写す先は _UNORM の型なので、形式は _UNORM と言う（sRGB の値のまま混ぜる。縁の明るさがわずかに違うだけ）。
		// 1 点のときは混ぜるものが無いので、そのまま写す
		if (m_sampleCount > 1) {
			m_context->ResolveSubresource(m_backBuffer.get(), 0, m_msaaTexture.get(), 0, kUnorm);
		}
		else {
			m_context->CopyResource(m_backBuffer.get(), m_msaaTexture.get());
		}
		return m_swapChain->Present(1, 0);
	}
}
