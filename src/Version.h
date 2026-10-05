#pragma once

// バージョン番号はここだけに書く。exe のバージョン情報（DisplayRescueService.rc と gui/DisplayRescue.rc）と version サブコマンドの両方がここを読む。
// .rc のリソースコンパイラからも読むので、C++ の構文（constexpr など）は使わずマクロだけで書く
#define DR_VERSION_MAJOR 1
#define DR_VERSION_MINOR 0
#define DR_VERSION_PATCH 1

// 数字のマクロを "1.0.0" のような文字列リテラルにする（# は 2 段階で展開しないと、マクロの名前そのものが文字列になる）
#define DR_STRINGIFY_(x) #x
#define DR_STRINGIFY(x) DR_STRINGIFY_(x)
#define DR_VERSION_STRING DR_STRINGIFY(DR_VERSION_MAJOR) "." DR_STRINGIFY(DR_VERSION_MINOR) "." DR_STRINGIFY(DR_VERSION_PATCH)
