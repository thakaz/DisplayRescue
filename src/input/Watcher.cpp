#include "input/Watcher.h"

#include <windows.h>
#include <wil/result.h>		//THROW_WIN32_IF / wil::ResultFromCaughtException

#include <chrono>
#include <format>
#include <iostream>
#include <utility>
#include <vector>

#include "common/Common.h"
#include "common/Log.h"
#include "config/Settings.h"
#include "ccd/Topology.h"

namespace {
	namespace ccd = rescue::ccd;
	namespace config = rescue::config;

	// コンソールとログの両方に出す（サービスから起動されたときはコンソールがどこにもつながっていない）
	void Report(std::wstring_view message) {
		std::wcout << message << L'\n';
		rescue::Log(std::format(L"[watch] {}", message));
	}

	// RegisterHotKey の登録 1 件。壊れるときに UnregisterHotKey で外す。
	// 番号（id）は 1 つのスレッドの中で重ならなければよい。WM_HOTKEY の wParam にこの番号が返ってくる
	class ScopedHotkey {
	public:
		// 登録できなければ例外（他のアプリが同じ組み合わせを持っていると ERROR_HOTKEY_ALREADY_REGISTERED）
		ScopedHotkey(int id, const config::Hotkey& hotkey) : m_id(id) {
			// MOD_NOREPEAT: 押しっぱなしでも 1 回だけ届く
			THROW_IF_WIN32_BOOL_FALSE(RegisterHotKey(nullptr, id, hotkey.modifiers | MOD_NOREPEAT, hotkey.vk));
		}
		~ScopedHotkey() {
			if (m_id != 0) UnregisterHotKey(nullptr, m_id);
		}

		// vector に入れるので移動はできるようにする。移動元は 0（登録なし）にして、二重に外さない
		ScopedHotkey(ScopedHotkey&& other) noexcept : m_id(std::exchange(other.m_id, 0)) {}
		ScopedHotkey& operator=(ScopedHotkey&&) = delete;
		ScopedHotkey(const ScopedHotkey&) = delete;
		ScopedHotkey& operator=(const ScopedHotkey&) = delete;

	private:
		int m_id = 0;	// 0 = 登録なし（0 も登録には使えるが、ここでは 1 から振る）
	};

	// 登録できた割り当てと、その登録
	struct ActiveBinding {
		config::Binding	binding;
		ScopedHotkey	registration;
	};

	//押されたトポロジにする。かかった時間も出す（ロック画面などで反応が遅いと感じたときの手がかり）
	void Switch(ccd::Topology topology) {
		const auto start = std::chrono::steady_clock::now();
		const LONG rc = ccd::ApplyTopology(topology);
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
		Report(std::format(L"{}: {} ({}、{} ms)", ccd::NameOf(topology), rescue::DescribeError(rc), rc, elapsed.count()));
	}
}

int rescue::input::RunWatch(HANDLE stopEvent) {
	try {
		const auto bindings = config::LoadBindings();

		//登録できた割り当て。RegisterHotKeyの番号はこの配列の添え字+1にする（0でも登録できるが、0を「無し」に使う慣例と紛れないように）
		//WM_HOTKEYのwParamでその番号が返ってくるのでそこから引き当てる。抜けるときは要素ごとに登録が外れる
		std::vector<ActiveBinding> active;
		active.reserve(bindings.size());

		for (const auto& binding : bindings) {
			const int id = static_cast<int>(active.size()) + 1;
			try {
				active.push_back({ binding, ScopedHotkey{ id, binding.hotkey } });
			}
			catch (...) {
				//登録できない組み合わせはその 1 件だけ飛ばす
				Report(std::format(L"{}を登録できません: {}", config::FormatHotkey(binding.hotkey), DescribeHresult(wil::ResultFromCaughtException())));
				continue;
			}
			std::wcout << std::format(L"{:<9} {}\n", ccd::NameOf(binding.topology), config::FormatHotkey(binding.hotkey));
		}
		//一つも登録できなければ
		THROW_WIN32_IF(ERROR_NOT_FOUND, active.empty());

		std::wcout << (stopEvent ? L"待ち受けています\n" : L"待ち受けています(Ctrl+Cで終了)\n");

		//メッセージループ。hWnd無しで登録したので、WM_HOTKEYはこのスレッドのメッセージキューに届く
		//GetMessageWはメッセージしか待てないので、イベントと一緒に待てるMsgWaitForMultipleObjectsを使う
		const DWORD handleCount = stopEvent ? 1 : 0;
		for (;;) {
			const DWORD wait = MsgWaitForMultipleObjects(handleCount, &stopEvent, FALSE, INFINITE, QS_ALLINPUT);
			THROW_LAST_ERROR_IF(wait == WAIT_FAILED);
			if (handleCount == 1 && wait == WAIT_OBJECT_0) {
				return ERROR_SUCCESS;	//止めてと言われた。登録は active が壊れるときに外れる
			}

			//WAIT_OBJECT_0 + handleCount : メッセージが来た（ハンドルの数をちょうど超えた値で知らせてくる）
			//MsgWaitは前回見てから新しく来たかで発生するので溜まっているものを全部取り出す。
			MSG message{};
			while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
				if (message.message != WM_HOTKEY) continue;

				//番号 = 添字 + 1 を逆にたどる。wParam が 0 なら -1 で巨大な値になり、下の範囲チェックで弾かれる
				const auto index = static_cast<size_t>(message.wParam) - 1;
				if (index < active.size()) Switch(active[index].binding.topology);
			}
		}
	}
	catch (...) {
		const HRESULT hr = wil::ResultFromCaughtException();
		Report(std::format(L"待ち受けを始められませんでした: {}", DescribeHresult(hr)));
		return ExitCodeFromHresult(hr);
	}
}
