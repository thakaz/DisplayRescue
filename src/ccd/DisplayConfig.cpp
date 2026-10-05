#include "ccd/DisplayConfig.h"

#include <wil/result.h>   // THROW_IF_WIN32_ERROR

namespace rescue::ccd
{
    DisplayConfig Query(UINT32 flags)
    {
        DisplayConfig config;

        // 2 回の呼び出しの間にモニタ構成が変わると ERROR_INSUFFICIENT_BUFFER になる。
        // その場合はサイズの取得からやり直す。
        LONG rc = ERROR_INSUFFICIENT_BUFFER;
        while (rc == ERROR_INSUFFICIENT_BUFFER)
        {
            UINT32 pathCount = 0;
            UINT32 modeCount = 0;
            rc = GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount);
            if (rc != ERROR_SUCCESS)
            {
                break;
            }

            config.paths.resize(pathCount);
            config.modes.resize(modeCount);

            rc = QueryDisplayConfig(flags,
                &pathCount, config.paths.data(),
                &modeCount, config.modes.data(),
                nullptr);  // QDC_DATABASE_CURRENT のときだけ必要
            if (rc == ERROR_SUCCESS)
            {
                // 実際に書き込まれた数に切り詰める
                config.paths.resize(pathCount);
                config.modes.resize(modeCount);
            }
        }

        THROW_IF_WIN32_ERROR(rc);
        return config;
    }

    std::optional<DISPLAYCONFIG_SOURCE_MODE> SourceModeOf(const DisplayConfig& config,
        const DISPLAYCONFIG_PATH_INFO& path)
    {
        const UINT32 idx = path.sourceInfo.modeInfoIdx;
        if (idx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || idx >= config.modes.size())
        {
            return std::nullopt;
        }
        const DISPLAYCONFIG_MODE_INFO& mode = config.modes[idx];
        if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
        {
            return std::nullopt;
        }
        return mode.sourceMode;
    }

    std::optional<DISPLAYCONFIG_TARGET_MODE> TargetModeOf(const DisplayConfig& config,
        const DISPLAYCONFIG_PATH_INFO& path)
    {
        const UINT32 idx = path.targetInfo.modeInfoIdx;
        if (idx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || idx >= config.modes.size())
        {
            return std::nullopt;
        }
        const DISPLAYCONFIG_MODE_INFO& mode = config.modes[idx];
        if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
        {
            return std::nullopt;
        }
        return mode.targetMode;
    }

    std::optional<TargetName> GetTargetName(const LUID& adapterId, UINT32 targetId)
    {
        // DisplayConfigGetDeviceInfo は「ヘッダ付き構造体」方式。
        // header.type で何を取るかを指定し、header.size に構造体全体のサイズを入れる。
        DISPLAYCONFIG_TARGET_DEVICE_NAME info{};
        info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        info.header.size = sizeof(info);
        info.header.adapterId = adapterId;
        info.header.id = targetId;

        if (DisplayConfigGetDeviceInfo(&info.header) != ERROR_SUCCESS)
        {
            return std::nullopt;
        }
        return TargetName{
            info.monitorFriendlyDeviceName,
            info.monitorDevicePath,
            info.outputTechnology,
            info.edidManufactureId,
            info.edidProductCodeId,
            info.connectorInstance,
        };
    }

    std::optional<std::wstring> GetSourceGdiName(const LUID& adapterId, UINT32 sourceId)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME info{};
        info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        info.header.size = sizeof(info);
        info.header.adapterId = adapterId;
        info.header.id = sourceId;

        if (DisplayConfigGetDeviceInfo(&info.header) != ERROR_SUCCESS)
        {
            return std::nullopt;
        }
        return std::wstring{ info.viewGdiDeviceName };
    }
}