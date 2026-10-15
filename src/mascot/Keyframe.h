#pragma once

#include <cstddef>
#include <span>

namespace rescue::mascot {
	// 時刻が、キーフレームの列のどこにあるか。
	// index 番目と index + 1 番目のキーフレームの間で、fraction（0〜1）だけ進んだところ。
	// fraction = 0 なら index 番目のキーフレームちょうど
	struct KeyframePosition {
		std::size_t index = 0;
		float fraction = 0.0f;
	};

	// times（キーフレームの時刻。小さい順。空でない）の中で、time がどこにあるかを返す。
	//   time が最初のキーフレームより前なら { 0, 0 }、最後のキーフレーム以降なら { 最後の番号, 0 }（はみ出したら端で止まる）
	KeyframePosition FindKeyframe(std::span<const float> times, float time);
}
