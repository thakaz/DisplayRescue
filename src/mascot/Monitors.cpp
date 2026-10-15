// モニタの一覧。マスコット（メインモニタ）と画面切り替えキーの案内（各モニタ）が使う。
// EnumDisplayMonitors は、OS が「モニタ 1 つにつき 1 回」呼ぶ関数（コールバック）に、LPARAM で自分の vector を渡せる。
// 窓のプロシージャ（WindowProc）と同じく「OS から呼ばれる関数」だが、こちらは引数で値を渡せるので、グローバル変数が要らない
#include "mascot/Monitors.h"

#include <wil/result.h>

#include <algorithm>
#include <tuple>

namespace {
	bool SameRect(const RECT& a, const RECT& b) {
		return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
	}
}

namespace rescue::mascot {
	bool MonitorArea::operator==(const MonitorArea& other) const {
		return SameRect(bounds, other.bounds) && SameRect(work, other.work) && primary == other.primary;
	}

	std::vector<MonitorArea> EnumerateMonitors() {
		std::vector<MonitorArea> monitors;
		// 捕まえる（[&]）もののないラムダは、ふつうの関数のアドレスに変えられる。だから EnumDisplayMonitors に渡せる。
		// 外の monitors は捕まえられないので、最後の引数（LPARAM）にアドレスを入れて渡し、中で元の型に戻す
		const auto addMonitor = [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
			MONITORINFO info{};
			info.cbSize = sizeof(info);
			if (GetMonitorInfoW(monitor, &info)) {
				reinterpret_cast<std::vector<MonitorArea>*>(data)->push_back(
					{ monitor, info.rcMonitor, info.rcWork, (info.dwFlags & MONITORINFOF_PRIMARY) != 0 });
			}
			return TRUE;	// 続ける（FALSE なら途中でやめる）
		};
		EnumDisplayMonitors(nullptr, nullptr, addMonitor, reinterpret_cast<LPARAM>(&monitors));

		// 呼ばれる順は決まっていないので、前と比べられるように左上から並べる
		std::ranges::sort(monitors, {}, [](const MonitorArea& m) { return std::tuple{ m.bounds.top, m.bounds.left }; });
		return monitors;
	}

	RECT PrimaryMonitorArea(const std::vector<MonitorArea>& monitors) {
		if (const auto primary = std::ranges::find(monitors, true, &MonitorArea::primary); primary != monitors.end()) return primary->bounds;
		return { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
	}
}
