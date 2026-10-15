#pragma once

#include <cstddef>
#include <cstdint>
#include <random>

namespace rescue::mascot {
	// 次に再生するモーションを乱数で選ぶ。同じものが 2 回続かないようにする（MotionSchedule が使う）
	class MotionPicker {
	public:
		// count: モーションの本数（1 以上）。乱数の種は std::random_device から取り、最初の 1 本も乱数で選んでおく
		explicit MotionPicker(std::size_t count);

		// 種を外から渡す版。同じ count と seed なら、どのプロセスでも同じ並びになる
		MotionPicker(std::size_t count, std::uint32_t seed);

		std::size_t Current() const noexcept { return m_current; }

		// 次の 1 本を選んで返す（Current も変わる）。今のもの以外を同じ確率で。1 本しか無ければいつも 0
		std::size_t Next();

	private:
		std::mt19937 m_engine;		// 乱数を作る機械（メルセンヌ・ツイスタ）。種を入れて作る
		std::size_t m_count;
		std::size_t m_current;
	};
}
