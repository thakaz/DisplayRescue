#include "mascot/ImageDecode.h"

#include <Windows.h>
#include <wincodec.h>		// WIC（Windows Imaging Component）
#include <wil/com.h>		// wil::com_ptr / wil::CoCreateInstance
#include <wil/resource.h>	// wil::CoInitializeEx
#include <wil/result.h>

#include <limits>

// CLSID_WICImagingFactory や GUID_WICPixelFormat32bppRGBA の実体はこのライブラリにある（リンカーに追加の .lib を指示する）
#pragma comment(lib, "windowscodecs.lib")

namespace rescue::mascot {

	DecodedImage DecodeImage(std::span<const std::byte> encoded) {
		// 1. COM を使う準備。WIC は COM の部品なので、CoCreateInstance の前にこのスレッドで CoInitializeEx が要る
		//   （D3D11 も COM の形の部品だが、CoCreateInstance で作らないので要らない）。
		// 戻り値は「抜けるときに CoUninitialize する」箱。捨てるとすぐに CoUninitialize されてしまう（WI_NODISCARD）。
		// 箱はこの関数で一番先に作るので、壊れるのは一番後。下の WIC の部品が全部解放されてから CoUninitialize になる。
		// すでに同じスレッドで MTA として初期化されていても、もう一度呼んでよい（呼んだ回数だけ CoUninitialize する）
		const auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);

		// 2. WIC のファクトリ
		const auto factory = wil::CoCreateInstance<IWICImagingFactory>(CLSID_WICImagingFactory);

		// 3. メモリ上のバイト列を「ストリーム」として見せる。WIC は読み込み元をストリームで受け取る（ファイルなら CreateDecoderFromFilename もある）。
		// InitializeFromMemory は書き込みもできる形（BYTE*）で受け取るが、読むだけなので const を外して渡す。
		// 中身はコピーされないので、encoded は stream を使い終わるまで生きている必要がある（この関数の中で使い終わる）。
		// 大きさは DWORD で渡すので、それを超えるものは断る
		THROW_HR_IF(E_INVALIDARG, encoded.size() > (std::numeric_limits<DWORD>::max)());
		wil::com_ptr<IWICStream> stream;
		THROW_IF_FAILED(factory->CreateStream(stream.put()));
		THROW_IF_FAILED(stream->InitializeFromMemory(
			reinterpret_cast<BYTE*>(const_cast<std::byte*>(encoded.data())), static_cast<DWORD>(encoded.size())));

		// 4. 形式を見分けて、合う読み手（デコーダ）を作る。PNG か JPEG かは先頭のバイトで WIC が判断する（nullptr = どの作者のデコーダでもよい）。
		// 画像は「フレーム」の集まり（GIF のアニメーションなど）。1 枚目だけ取る
		wil::com_ptr<IWICBitmapDecoder> decoder;
		THROW_IF_FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put()));
		wil::com_ptr<IWICBitmapFrameDecode> frame;
		THROW_IF_FAILED(decoder->GetFrame(0, frame.put()));

		// 5. 画素の形をそろえる。ファイルによって RGB 3 バイト・パレット・16 ビットなどばらばらなので、
		// コンバータに「1 画素 4 バイト、RGBA の順」へ変えてもらう（D3D の DXGI_FORMAT_R8G8B8A8_UNORM と同じ並び）。
		// ここではまだ変換しない（CopyPixels のときに変換する）
		wil::com_ptr<IWICFormatConverter> converter;
		THROW_IF_FAILED(factory->CreateFormatConverter(converter.put()));
		THROW_IF_FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));

		// 6. 大きさを聞いてバッファを用意し、画素を写す。
		// stride は「1 行が何バイトか」（幅 × 4）。CopyPixels の nullptr は「画像全体」
		DecodedImage image;
		THROW_IF_FAILED(converter->GetSize(&image.width, &image.height));
		const UINT stride = image.width * 4;
		const UINT64 size = static_cast<UINT64>(stride) * image.height;
		THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE), size > (std::numeric_limits<UINT>::max)());
		image.pixels.resize(static_cast<size_t>(size));
		THROW_IF_FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(size), reinterpret_cast<BYTE*>(image.pixels.data())));

		// 抜けるときに作った順と逆に壊れる: converter → frame → decoder → stream → factory → coInit（CoUninitialize）
		return image;
	}
}
