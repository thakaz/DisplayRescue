#pragma once

#include <Windows.h>

#include <string>
#include <string_view>

namespace rescue {
	//大文字小文字を無視して比較する
	bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b);

	//Win32 エラーコードを名前にする（例 "ERROR_ACCESS_DENIED"）。
	//表に無いコードは 16 進と Windows の説明文（例 "0x00000581 ホット キーは既に登録されています。"）
	std::wstring DescribeError(LONG rc);

	//HRESULTを名前にする。Win32エラーから作られたものは元のWin32エラー名、それ以外は16進。
	std::wstring DescribeHresult(HRESULT hr);

	//HRESULT を終了コードにする。Win32 エラーから作られたものは元の Win32 エラーコード、それ以外は HRESULT の値そのまま
	int ExitCodeFromHresult(HRESULT hr);
}
