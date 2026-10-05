#include "common/FileVersion.h"

#include <windows.h>

#include <format>
#include <vector>

#include "Version.h"

#pragma comment(lib, "version.lib")	// GetFileVersionInfoW / VerQueryValueW

namespace rescue {

	std::wstring FileVersion::ToString() const {
		return std::format(L"{}.{}.{}", major, minor, patch);
	}

	FileVersion CurrentVersion() {
		return { DR_VERSION_MAJOR, DR_VERSION_MINOR, DR_VERSION_PATCH };
	}

	std::optional<FileVersion> ReadFileVersion(const std::filesystem::path& file) {
		// バージョン情報はリソースの塊として読み、その中の固定部分（VS_FIXEDFILEINFO）を取り出す
		DWORD ignored = 0;
		const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
		if (size == 0) return std::nullopt;	// ファイルが無い・バージョン情報が無い

		std::vector<std::byte> data(size);
		if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) return std::nullopt;

		// L"\\" = 固定部分。info は data の中を指すので、data より長く使わない
		VS_FIXEDFILEINFO* info = nullptr;
		UINT length = 0;
		if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || length < sizeof(VS_FIXEDFILEINFO)) {
			return std::nullopt;
		}
		// FILEVERSION a, b, c, d は dwFileVersionMS = a:b、dwFileVersionLS = c:d（上位 16 ビット:下位 16 ビット）
		return FileVersion{ HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS) };
	}
}
