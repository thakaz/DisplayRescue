#pragma once

namespace rescue::service {
	// サービス名 SCMへの登録名でsc.exe control DisplayRescue 130などで指定する名前。
	// サービス本体(Service.cpp)とインストーラ(Installer.cpp)の両方が使う
	inline constexpr wchar_t kServiceName[] = L"DisplayRescue";

	// サービスに送れるユーザー定義のコントロールコード（sc.exe control DisplayRescue <番号> でも送れる）。
	// 128 / 129 は以前の切り替え要求（拡張 / 複製）で使っていた番号なので使わない（古いサービスに送って画面が変わらないように）
	inline constexpr unsigned long kControlReload = 130;	// キーの割り当てを読み直す（待ち受け役を置き直す）。bind / unbind が送る

	// SCM(サービスコントロールマネージャ)から"DisplayRescueService.exe service"として起動された時の入り口
	int RunService();

	// サービスの状態（GUI の表示用）
	enum class ServiceState {
		NotInstalled,	// 登録されていない
		Stopped,
		Running,
		Pending,		// 起動中・停止中など、切り替わりの途中
		Unknown,		// 調べられなかった
	};
	ServiceState QueryServiceState();

	// サービスに「キーの割り当てを読み直して」と頼む。サービスが無い・止まっているときは false（何もしない）
	bool RequestServiceReload();
}
