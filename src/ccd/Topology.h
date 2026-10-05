#pragma once

#include <windows.h>

#include <optional>
#include <span>
#include <string_view>

namespace rescue::ccd {
	// 切り替え先の構成。SetDisplayConfig のトポロジ指定（SDC_TOPOLOGY_*）に対応する
	enum class Topology {
		Extend,		// 拡張
		Clone,		// 複製
		Internal,	// メインのみ
		External,	// サブのみ
	};

	// 全トポロジ。順番は固定（一覧表示や既定の割り当てはこの順）
	std::span<const Topology> AllTopologies();

	// "extend" / "clone" / "internal" / "external"。CLI の引数やレジストリの値の名前に使う
	std::wstring_view NameOf(Topology topology);

	// 名前 → Topology。大文字小文字は問わない。不明なら nullopt
	std::optional<Topology> ParseTopology(std::wstring_view name);

	// 検証（SDC_VALIDATE）してから適用（SDC_APPLY）する。戻り値は Win32 エラーコード（成功は ERROR_SUCCESS）。
	// 構成は配列で渡さず、Windows が覚えている構成（持続化 DB）から組み立ててもらう
	LONG ApplyTopology(Topology topology);
}
