#include "common/Common.h"

#include <wtsapi32.h>		// WTSQuerySessionInformationW（wil/resource.h より先に include すると wil::unique_wtsmem_ptr が使える）
#include <wil/resource.h>	// wil::unique_hlocal_string / unique_wtsmem_ptr
#include <wil/result.h>

#include <cwchar>			// std::wcstoul
#include <format>

#pragma comment(lib, "wtsapi32.lib")

namespace {
	// Windows の説明文（FormatMessageW）。取れなければ空。末尾の改行は落とす
	std::wstring SystemMessage(DWORD code) {
		// ALLOCATE_BUFFER: 大きさを OS に決めてもらい、LocalFree で返す領域に書いてもらう。
		//   lpBuffer の型は LPWSTR だが、このときだけ「ポインタのアドレス」を渡す決まりなのでキャストする
		// IGNORE_INSERTS: 説明文の中の %1 などを埋めようとしない（埋める引数を渡さないため）
		LPWSTR raw = nullptr;
		const DWORD length = FormatMessageW(
			FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, code, 0, reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
		const wil::unique_hlocal_string buffer{ raw };	// 受け取ったらすぐ箱に入れる（LocalFree で返す）
		if (length == 0) return {};

		std::wstring_view text{ buffer.get(), length };
		while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) {
			text.remove_suffix(1);
		}
		return std::wstring{ text };
	}
}

namespace rescue {

	std::optional<HANDLE> ParseInheritedHandle(const wchar_t* text)
	{
		wchar_t* end = nullptr;
		const unsigned long value = std::wcstoul(text, &end, 10);	//10進で読む。endは読めなかった最初の文字を指す
		if (end == text || *end != L'\0' || value == 0) return std::nullopt;
		return ULongToHandle(value);
	}

	//Windows 7 / Server 2008 R2 は LOCK と UNLOCK が逆になる不具合があるが、対象外
	bool IsSessionLocked(DWORD sessionId)
	{
		//バッファの型は LPWSTR として宣言されているが、WTSSessionInfoEx を頼むと中身は WTSINFOEXW になる。
		//WTSFreeMemory で返す必要があるので、受け取ったらすぐ箱に入れる
		LPWSTR buffer = nullptr;
		DWORD bytes = 0;
		THROW_IF_WIN32_BOOL_FALSE(WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId, WTSSessionInfoEx, &buffer, &bytes));
		const wil::unique_wtsmem_ptr<WTSINFOEXW> info{ reinterpret_cast<WTSINFOEXW*>(buffer) };

		THROW_WIN32_IF(ERROR_INVALID_DATA, info->Level != 1);	//今の Windows は Level 1 しか返さない
		return info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
	}

	bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b)
	{
		return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
	}

	std::wstring DescribeError(LONG rc)
	{
        switch (rc)
        {
        case ERROR_SUCCESS:                             return L"ERROR_SUCCESS";
        case ERROR_INVALID_PARAMETER:                   return L"ERROR_INVALID_PARAMETER";
        case ERROR_NOT_SUPPORTED:                       return L"ERROR_NOT_SUPPORTED";
        case ERROR_ACCESS_DENIED:                       return L"ERROR_ACCESS_DENIED";
        case ERROR_GEN_FAILURE:                         return L"ERROR_GEN_FAILURE";
        case ERROR_BAD_CONFIGURATION:                   return L"ERROR_BAD_CONFIGURATION";
        case ERROR_INSUFFICIENT_BUFFER:                 return L"ERROR_INSUFFICIENT_BUFFER";
        case ERROR_FAILED_SERVICE_CONTROLLER_CONNECT:   return L"ERROR_FAILED_SERVICE_CONTROLLER_CONNECT";
        case ERROR_BAD_ARGUMENTS:                       return L"ERROR_BAD_ARGUMENTS";
        case ERROR_NOT_FOUND:                           return L"ERROR_NOT_FOUND";
        case ERROR_NO_TOKEN:                            return L"ERROR_NO_TOKEN";
        case ERROR_NO_SUCH_LOGON_SESSION:               return L"ERROR_NO_SUCH_LOGON_SESSION";
        case ERROR_PRIVILEGE_NOT_HELD:                  return L"ERROR_PRIVILEGE_NOT_HELD";
        case ERROR_TIMEOUT:                             return L"ERROR_TIMEOUT";
        case ERROR_OPERATION_ABORTED:                   return L"ERROR_OPERATION_ABORTED";
        case ERROR_SHARING_VIOLATION:                   return L"ERROR_SHARING_VIOLATION";
        case ERROR_ALREADY_EXISTS:                      return L"ERROR_ALREADY_EXISTS";
        case ERROR_CANCELLED:                           return L"ERROR_CANCELLED";
        case ERROR_HOTKEY_ALREADY_REGISTERED:           return L"ERROR_HOTKEY_ALREADY_REGISTERED";
        case ERROR_SERVICE_EXISTS:                      return L"ERROR_SERVICE_EXISTS";
        case ERROR_SERVICE_DOES_NOT_EXIST:              return L"ERROR_SERVICE_DOES_NOT_EXIST";
        case ERROR_SERVICE_NOT_ACTIVE:                  return L"ERROR_SERVICE_NOT_ACTIVE";
        case ERROR_SERVICE_ALREADY_RUNNING:             return L"ERROR_SERVICE_ALREADY_RUNNING";
        case ERROR_SERVICE_MARKED_FOR_DELETE:           return L"ERROR_SERVICE_MARKED_FOR_DELETE";
        default:
        {
            const auto code = static_cast<unsigned long>(rc);
            const auto message = SystemMessage(code);
            return message.empty() ? std::format(L"0x{:08X}", code) : std::format(L"0x{:08X} {}", code, message);
        }
        }
	}

    std::wstring DescribeHresult(HRESULT hr)
    {
        if (HRESULT_FACILITY(hr) == FACILITY_WIN32)
        {
            return DescribeError(HRESULT_CODE(hr));
        }
        return std::format(L"0x{:08X}", static_cast<unsigned long>(hr));
    }

    int ExitCodeFromHresult(HRESULT hr)
    {
        return HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : static_cast<int>(hr);
    }
}
