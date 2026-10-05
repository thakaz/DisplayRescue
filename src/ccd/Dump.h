#pragma once

#include "ccd/DisplayConfig.h"

namespace rescue::ccd {
    // Query 結果を人間が読める形でコンソールに出す
    void PrintDisplayConfig(const DisplayConfig& config);
}