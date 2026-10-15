#include "mascot/MotionPicker.h"

namespace rescue::mascot {

	// 1. 種を選ばない版は、種を作って下の版に任せる（委譲コンストラクタ）
	MotionPicker::MotionPicker(std::size_t count) : MotionPicker(count, std::random_device{}()) {}

	// 2. 最初の 1 本も乱数で選ぶ
	MotionPicker::MotionPicker(std::size_t count, std::uint32_t seed) : m_engine{ seed }, m_count{ count }, m_current{ 0 } {
		std::uniform_int_distribution<std::size_t> distribution{ 0, m_count - 1 };
		m_current = distribution(m_engine);
	}

	std::size_t MotionPicker::Next() {
		if (m_count <= 1) return m_current;

		// 3. 今のもの以外から選ぶ。今のものを除いた count - 1 個から選び、今のもの以上なら 1 つずらす
		std::uniform_int_distribution<std::size_t> distribution{ 0, m_count - 2 };
		const std::size_t pick = distribution(m_engine);
		m_current = pick >= m_current ? pick + 1 : pick;
		return m_current;
	}
}
