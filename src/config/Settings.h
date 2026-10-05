#pragma once

#include <optional>
#include <span>
#include <vector>

#include "ccd/Topology.h"
#include "config/Hotkey.h"

namespace rescue::config {
	// 「このキーが押されたらこのトポロジにする」1 件
	struct Binding {
		ccd::Topology	topology;
		Hotkey			hotkey;

		bool operator==(const Binding&) const = default;
	};

	// 何も設定していないときの割り当て（Ctrl+Alt+F8〜F11）。1.0.0 は F9〜F12 だったが、F12 はデバッガ用に予約されていてホットキーに登録すべきでない（RegisterHotKey のドキュメント）ので 1.0.1 で 1 つずらした
	std::vector<Binding> DefaultBindings();

	// HKLM\SOFTWARE\DisplayRescue\Bindings から読む。一般ユーザーでも読める。
	//   キーが無い             → DefaultBindings()
	//   キーがある             → そこにある値だけが割り当て（値が無い action は割り当てなし）
	//   読めない値（"Ctrl+Alt+F9" 形式でない、REG_SZ でない など）→ その 1 件だけ飛ばす（他の割り当てまで道連れにしない）
	// キーを開けない（アクセス拒否など）ときは例外
	std::vector<Binding> LoadBindings();

	//割り当てをまとめて置き換える（bindings に無い action は割り当てなしにする）。管理者でないとアクセス拒否の例外。
	//同じキーが 2 つの action にあれば何も書かず、後ろのほうの action を返す。書けたら nullopt。
	//書き込みはすべてこれを通る。GUI は 4 件をまとめて渡す（1 件ずつだと、2 つのキーを入れ替えるときに途中の状態が「重なり」になる）
	std::optional<ccd::Topology> ReplaceBindings(std::span<const Binding> bindings);

	//1 件書き込む（同じ action の割り当ては置き換える）。今の割り当て（LoadBindings。設定が無ければ既定）に 1 件足して ReplaceBindings する。
	//同じキーが別の action に割り当て済みなら何も書かずその action を返す。書けたら nullopt。管理者でないとアクセス拒否の例外
	std::optional<ccd::Topology> SaveBinding(const Binding& binding);

	//1 件消す。今の割り当てから 1 件除いて ReplaceBindings する（設定が無ければ、既定から 1 件除いたものになる）。
	//元から割り当てがなければ false。管理者でないとアクセス拒否の例外
	bool RemoveBinding(ccd::Topology topology);

	//設定をすべて消す（HKLM\SOFTWARE\DisplayRescue ごと）。uninstall 用。
	//管理者でないとアクセス拒否の例外。もともと無ければ何もしない
	void DeleteAllSettings();

}
