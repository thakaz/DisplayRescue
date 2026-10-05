#pragma once

#include <compare>
#include <filesystem>
#include <optional>
#include <string>

namespace rescue {
	// 版（major.minor.patch）。大小を比べられる
	struct FileVersion {
		unsigned major = 0;
		unsigned minor = 0;
		unsigned patch = 0;

		auto operator<=>(const FileVersion&) const = default;

		// "0.1.0"
		std::wstring ToString() const;
	};

	// この exe 自身の版（src/Version.h）
	FileVersion CurrentVersion();

	// exe に埋め込んだバージョン情報（.rc の VERSIONINFO の FILEVERSION）を読む。ファイルが無い・情報が無ければ nullopt
	std::optional<FileVersion> ReadFileVersion(const std::filesystem::path& file);
}
