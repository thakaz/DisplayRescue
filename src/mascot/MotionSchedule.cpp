#include "mascot/MotionSchedule.h"

#include <algorithm>

namespace {
	// 1. 乱数で選ぶモーション（待機モーション以外）の番号を並べる
	std::vector<std::size_t> OthersOf(std::size_t count, std::optional<std::size_t> idle) {
		std::vector<std::size_t> others;
		for (std::size_t i = 0; i < count; ++i) {
			if (i != idle) others.push_back(i);
		}
		return others;
	}
}

namespace rescue::mascot {

	// 2. メンバーは宣言の順に作られる。m_idle は m_durations を、m_others は m_idle を、m_picker は m_others を使うので、この順に宣言してある
	MotionSchedule::MotionSchedule(std::vector<float> durations, std::uint32_t seed, std::optional<std::size_t> idle, double idleSeconds)
		: m_durations{ std::move(durations) }, m_seed{ seed }, m_idle{ idle && *idle < m_durations.size() ? idle : std::nullopt },
		m_idleSeconds{ (std::max)(idleSeconds, static_cast<double>(kMinimumDuration)) },
		m_others{ OthersOf(m_durations.size(), m_idle) },
		m_picker{ (std::max)(m_others.size(), std::size_t{ 1 }), seed } {
		// 短すぎる長さを引き上げる
		for (float& duration : m_durations) duration = (std::max)(duration, kMinimumDuration);
		Restart();
	}

	// 3. その日の 0 秒目の状態に戻す（同じ種なら、どのプロセスでも同じ並びになる）
	void MotionSchedule::Restart() {
		m_picker = MotionPicker{ (std::max)(m_others.size(), std::size_t{ 1 }), m_seed };
		m_inIdle = m_idle.has_value();
		m_start = 0.0;
	}

	double MotionSchedule::CurrentLength() const {
		return m_inIdle ? m_idleSeconds : m_durations[m_others[m_picker.Current()]];
	}

	ScheduledMotion MotionSchedule::At(double secondsOfDay) {
		// 4. 時計が戻ったら、最初の 1 本からたどり直す
		if (secondsOfDay < m_start) Restart();
		// 5. 待機モーションしかなければ、ずっと待機
		if (m_others.empty()) return { *m_idle, 0.0 };

		// 6. 今の区切りが終わっていたら次へ。待機があれば「待機 → 乱数の 1 本 → 待機 …」と交互に
		while (m_start + CurrentLength() <= secondsOfDay) {
			m_start += CurrentLength();
			if (!m_idle) {
				m_picker.Next();
			}
			else if (m_inIdle) {
				m_inIdle = false;
			}
			else {
				m_inIdle = true;
				m_picker.Next();
			}
		}
		return { m_inIdle ? *m_idle : m_others[m_picker.Current()], m_start };
	}
}
