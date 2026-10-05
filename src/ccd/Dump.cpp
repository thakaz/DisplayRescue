#include "ccd/Dump.h"

#include <format>
#include <iostream>
#include <string>
#include <string_view>

namespace
{
    namespace ccd = rescue::ccd;

    std::wstring FormatLuid(const LUID& luid)
    {
        return std::format(L"{:08X}:{:08X}", static_cast<unsigned long>(luid.HighPart), luid.LowPart);
    }

    std::wstring FormatRefresh(const DISPLAYCONFIG_RATIONAL& r)
    {
        if (r.Denominator == 0)
        {
            return L"?";
        }
        return std::format(L"{:.2f}Hz", static_cast<double>(r.Numerator) / r.Denominator);
    }

    // 戻り値はリテラルを指す view なので寿命の問題はない
    std::wstring_view OutputTechnologyName(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY tech)
    {
        switch (tech)
        {
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15:                 return L"VGA";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI:                  return L"DVI";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI:                 return L"HDMI";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: return L"DisplayPort";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST:             return L"Miracast";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED:       return L"Indirect(有線)";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL:     return L"Indirect(仮想)";
            // ここから下が SDC_TOPOLOGY_INTERNAL の「内蔵」扱いになる種類
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS:                 return L"LVDS[内蔵]";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED: return L"eDP[内蔵]";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED:         return L"UDI[内蔵]";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:             return L"Internal[内蔵]";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER:                return L"その他";
        default:                                                   return L"(不明)";
        }
    }

    void PrintActivePath(const ccd::DisplayConfig& config, size_t index, const DISPLAYCONFIG_PATH_INFO& path)
    {
        const auto& src = path.sourceInfo;
        const auto& tgt = path.targetInfo;

        std::wcout << std::format(L"path[{}]有効\n", index);

        // --- source（GPU 側の描画面）---
        const auto gdiName = ccd::GetSourceGdiName(src.adapterId, src.id);
        std::wcout << std::format(L"  source : adapter {}  id {}  {}\n",
            FormatLuid(src.adapterId), src.id,
            gdiName.value_or(L"(名前なし)"));

        if (const auto mode = ccd::SourceModeOf(config, path))
        {
            const bool primary = mode->position.x == 0 && mode->position.y == 0;
            std::wcout << std::format(L"           mode[{}] {}x{} at ({}, {}){}\n",
                src.modeInfoIdx, mode->width, mode->height,
                mode->position.x, mode->position.y,
                primary ? L"  ← メイン" : L"");
        }

        // --- target（コネクタとモニタ）---
        const auto name = ccd::GetTargetName(tgt.adapterId, tgt.id);
        std::wcout << std::format(L"  target : adapter {}  id {}  {}  \"{}\"\n",
            FormatLuid(tgt.adapterId), tgt.id,
            OutputTechnologyName(tgt.outputTechnology),
            name ? name->friendlyName : std::wstring{});
        if (name)
        {
            std::wcout << std::format(L"           path  {}\n", name->devicePath);
            std::wcout << std::format(L"           edid  {:04X}:{:04X}  connector#{}\n",
                name->edidManufactureId, name->edidProductCodeId,
                name->connectorInstance);
        }

        if (const auto mode = ccd::TargetModeOf(config, path))
        {
            const auto& sig = mode->targetVideoSignalInfo;
            std::wcout << std::format(L"           mode[{}] {}x{} @ {}\n",
                tgt.modeInfoIdx, sig.activeSize.cx, sig.activeSize.cy,
                FormatRefresh(sig.vSyncFreq));
        }
        std::wcout << L"\n";
    }

    // QDC_ALL_PATHS のときに大量に出る無効な path は 1 行で出す
    void PrintInactivePath(size_t index, const DISPLAYCONFIG_PATH_INFO& path)
    {
        const auto& tgt = path.targetInfo;
        const auto  name = ccd::GetTargetName(tgt.adapterId, tgt.id);
        std::wcout << std::format(L"path[{}]無効  source {} -> target {}  {}  \"{}\"  使用可能={}\n",
            index, path.sourceInfo.id, tgt.id,
            OutputTechnologyName(tgt.outputTechnology),
            name ? name->friendlyName : std::wstring{},
            tgt.targetAvailable ? L"はい" : L"いいえ");
    }
}

void rescue::ccd::PrintDisplayConfig(const DisplayConfig& config)
{
    std::wcout << std::format(L"path数: {}  mode数: {}\n\n", config.paths.size(), config.modes.size());

    for (size_t i = 0; i < config.paths.size(); ++i)
    {
        const auto& path = config.paths[i];
        if (path.flags & DISPLAYCONFIG_PATH_ACTIVE)
        {
            PrintActivePath(config, i, path);
        }
        else
        {
            PrintInactivePath(i, path);
        }
    }
}