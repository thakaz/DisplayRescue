#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace rescue::mascot {
	// 画像を画素の並びに戻したもの
	struct DecodedImage {
		unsigned int width = 0;
		unsigned int height = 0;
		std::vector<std::byte> pixels;	// 1 画素 4 バイト（R, G, B, A の順）。上の行から、左から。大きさは width * height * 4
	};

	// PNG / JPEG などの画像ファイルの中身（メモリ上のバイト列）を、WIC で画素の並びに戻す。
	// 形式はバイト列の先頭から WIC が見分ける。アニメーションなど複数枚あるものは 1 枚目だけ。失敗は例外
	DecodedImage DecodeImage(std::span<const std::byte> encoded);
}
