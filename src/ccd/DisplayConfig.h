#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace rescue::ccd
{
    // QueryDisplayConfig の結果。path は mode を index で参照する。
    struct DisplayConfig
    {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths;
        std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    };

    // flags: QDC_ONLY_ACTIVE_PATHS または QDC_ALL_PATHS
    // 失敗は WIL の例外（wil::ResultException。HRESULT は Win32 エラーから作ったもの）
    DisplayConfig Query(UINT32 flags);

    // path が参照している mode を取り出す。参照が無効なら nullopt。
    std::optional<DISPLAYCONFIG_SOURCE_MODE> SourceModeOf(const DisplayConfig& config,
        const DISPLAYCONFIG_PATH_INFO& path);
    std::optional<DISPLAYCONFIG_TARGET_MODE> TargetModeOf(const DisplayConfig& config,
        const DISPLAYCONFIG_PATH_INFO& path);

    // target（モニタ）の識別情報
    struct TargetName
    {
        std::wstring                          friendlyName;   // "DELL U2720Q" など（EDID 由来）
        std::wstring                          devicePath;     // 安定した識別子
        DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY outputTechnology;
        UINT16                                edidManufactureId;
        UINT16                                edidProductCodeId;
        UINT32                                connectorInstance;
    };

    std::optional<TargetName>   GetTargetName(const LUID& adapterId, UINT32 targetId);
    std::optional<std::wstring> GetSourceGdiName(const LUID& adapterId, UINT32 sourceId);  // "\\.\DISPLAY1"
}