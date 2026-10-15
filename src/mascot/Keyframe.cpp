#include "mascot/Keyframe.h"

#include <algorithm>	// std::ranges::upper_bound
#include <iterator>

namespace rescue::mascot {
	KeyframePosition FindKeyframe(std::span<const float> times, float time) {
		// 1. 端を先に片づける。最初より前は最初で、最後以降は最後で止める。
		// ここで返しておけば、2. では time の前後に必ずキーフレームがある
		if (time <= times.front()) return { 0, 0.0f };
		if (time >= times.back()) return { times.size() - 1, 0.0f };

		// 2. time より大きい最初の要素。その 1 つ前が time 以下の最後の要素
		const auto next = std::ranges::upper_bound(times, time);
		const auto previous = next - 1;

		// 3. 前のキーフレームの番号と、次までの間のどこにいるか（0〜1）
		const auto index = static_cast<std::size_t>(std::ranges::distance(times.begin(), previous));
		const float fraction = (time - *previous) / (*next - *previous);
		return { index, fraction };
	}
}
