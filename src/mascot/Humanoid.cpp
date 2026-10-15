#include "mascot/Humanoid.h"

#include <winrt/windows.foundation.collections.h>
#include <winrt/windows.data.json.h>

#pragma comment(lib,"windowsapp.lib")

namespace rescue::mascot {

    HumanBones ParseHumanBones(std::string_view json) {
        using winrt::Windows::Data::Json::JsonObject;

        const JsonObject root = JsonObject::Parse(winrt::to_hstring(json));
        HumanBones bones;
        if (!root.HasKey(L"humanoid")) return bones;
        const JsonObject humanoid = root.GetNamedObject(L"humanoid");
        if (!humanoid.HasKey(L"humanBones")) return bones;

        // 1. "hips": { "node": 3 } の組を 1 つずつ（名前とその値を構造化束縛で受ける）
        for (const auto& [name, value] : humanoid.GetNamedObject(L"humanBones")) {
            const auto node = static_cast<std::size_t>(value.GetObject().GetNamedNumber(L"node"));
            // 2. unordered_map に入れる。bones[鍵] = 値 は、無ければ作って入れ、あれば上書きする
            bones[std::wstring{ name }] = node;
        }
        return bones;
    }
}