#include "ccd/Topology.h"

#include <algorithm>
#include <array>

#include "common/Common.h"

namespace {
	using rescue::ccd::Topology;

	struct TopologyInfo {
		Topology			topology;
		std::wstring_view	name;
		UINT32				flag;
	};

	// enum の並びと同じ順に置く（Info() は添字で引く）
	constexpr std::array kTopologyInfo{
		TopologyInfo{ Topology::Extend,		L"extend",		SDC_TOPOLOGY_EXTEND },
		TopologyInfo{ Topology::Clone,		L"clone",		SDC_TOPOLOGY_CLONE },
		TopologyInfo{ Topology::Internal,	L"internal",	SDC_TOPOLOGY_INTERNAL },
		TopologyInfo{ Topology::External,	L"external",	SDC_TOPOLOGY_EXTERNAL },
	};

	constexpr std::array kAllTopologies{ Topology::Extend, Topology::Clone, Topology::Internal, Topology::External };

	// 並びがずれていたらコンパイルを止める
	static_assert(std::ranges::all_of(std::array{ 0, 1, 2, 3 }, [](int i) { return static_cast<int>(kTopologyInfo[i].topology) == i; }));
	static_assert(kAllTopologies.size() == kTopologyInfo.size());

	const TopologyInfo& Info(Topology topology) {
		return kTopologyInfo[static_cast<size_t>(topology)];
	}
}

namespace rescue::ccd {

	std::span<const Topology> AllTopologies() {
		return kAllTopologies;
	}

	std::wstring_view NameOf(Topology topology) {
		return Info(topology).name;
	}

	std::optional<Topology> ParseTopology(std::wstring_view name) {
		const auto it = std::ranges::find_if(kTopologyInfo, [name](const TopologyInfo& t) { return EqualsIgnoreCase(t.name, name); });
		if (it == kTopologyInfo.end()) return std::nullopt;
		return it->topology;
	}

	LONG ApplyTopology(Topology topology) {
		const UINT32 flag = Info(topology).flag;
		// 検証で通らなければ何も変えずに返す
		if (const LONG rc = SetDisplayConfig(0, nullptr, 0, nullptr, flag | SDC_VALIDATE); rc != ERROR_SUCCESS) {
			return rc;
		}
		return SetDisplayConfig(0, nullptr, 0, nullptr, flag | SDC_APPLY);
	}
}
