#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "mascot/MotionPicker.h"

namespace rescue::mascot {
	// 「その日の何秒目か」から、「今どのモーションの何秒目か」を決める予定表。
	// 0 秒目から、MotionPicker（種は seed）の選ぶ順にモーションを並べ、長さの分ずつ進めていくと、ある時刻にどれが流れているかが決まる。
	// 同じ長さの並び・同じ seed なら、どのプロセスでも同じ答えになる。だから 2 つのマスコット（パスワード欄用とロック画像用）が
	// それぞれ自分で計算しても、同じ瞬間に同じ姿勢になる（見える側が入れ替わっても跳ばない）
	//
	// 待機モーション（idle）があれば、並びは「待機（idleSeconds 秒）→ 乱数の 1 本 → 待機 → 乱数の 1 本 …」。
	// 待機の間は待機モーションを繰り返す（Model の再生は長さを超えると頭から繰り返す）。乱数で選ぶのは待機モーション以外から。
	// 待機モーションが無ければ、乱数の 1 本を続けて再生する。待機モーションしか無ければ、ずっと待機
	struct ScheduledMotion {
		std::size_t index = 0;	// どのモーションか（durations の番号）
		double start = 0.0;		// そのモーションが、その日の何秒目に始まったか

		bool operator==(const ScheduledMotion&) const = default;
	};

	class MotionSchedule {
	public:
		// durations: モーションごとの長さ（秒。1 本以上）。0 や負は短すぎて先へ進めないので、kMinimumDuration に引き上げる。
		// seed: 乱数の種（Mascot は日付から作る）。
		// idle: 待機モーションの番号（durations の番号。無ければ nullopt）。idleSeconds: 1 回の待機の長さ（秒。kMinimumDuration より短ければ引き上げる）
		MotionSchedule(std::vector<float> durations, std::uint32_t seed, std::optional<std::size_t> idle = std::nullopt, double idleSeconds = 30.0);

		// secondsOfDay の時点で流れているモーション。前に聞いたときから進めて求める（毎フレーム呼んでも、たどるのは差の分だけ）。
		// 前に聞いた時刻より前を聞かれたら（時計が戻された）、0 秒目からたどり直す
		ScheduledMotion At(double secondsOfDay);

		static constexpr float kMinimumDuration = 0.1f;

	private:
		// 今の区切り（待機か、乱数の 1 本か）の長さ
		double CurrentLength() const;
		// 0 秒目・最初の区切りに戻す（コンストラクタと、時計が戻ったとき）
		void Restart();

		// 宣言の順に作られる。m_others は m_durations と idle から作り、m_picker は m_others の数を使って作るので、この順に置く
		std::vector<float> m_durations;
		std::uint32_t m_seed;
		std::optional<std::size_t> m_idle;
		double m_idleSeconds;
		std::vector<std::size_t> m_others;	// 乱数で選ぶモーションの番号（待機モーション以外）。空なら待機だけ
		MotionPicker m_picker;				// m_others の何番目か（Current）と、次を選ぶ乱数
		bool m_inIdle = false;				// 今の区切りが待機か
		double m_start = 0.0;				// 今の区切りが始まった時刻（その日の何秒目か）
	};
}
