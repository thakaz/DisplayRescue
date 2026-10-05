#pragma once

// 設定ダイアログ（DisplayRescue.rc）の ID。.rc とコードの両方が読む

#define IDD_SETTINGS            101

// アイコン。操作ごとのアイコンは ccd::AllTopologies() の順に連番にする（コードは「先頭 + 添字」で引く）
#define IDI_DISPLAY_RESCUE      201
#define IDI_MODE_FIRST          202
#define IDI_EXTEND              202
#define IDI_CLONE               203
#define IDI_INTERNAL            204
#define IDI_EXTERNAL            205

// 番号の要らないコントロール（ラベルなど）。winres.h にある定義だが、windows.h だけでは入らないので自分で置く
#ifndef IDC_STATIC
#define IDC_STATIC              (-1)
#endif

// キーの入力欄・「外す」ボタン・操作のアイコン。ccd::AllTopologies() の順（拡張・複製・メインのみ・サブのみ）に連番にする。
// コードは「先頭 + 添字」で引くので、順番と連番を崩さない
#define IDC_HOTKEY_FIRST        1001
#define IDC_HOTKEY_EXTEND       1001
#define IDC_HOTKEY_CLONE        1002
#define IDC_HOTKEY_INTERNAL     1003
#define IDC_HOTKEY_EXTERNAL     1004

#define IDC_CLEAR_FIRST         1011
#define IDC_CLEAR_EXTEND        1011
#define IDC_CLEAR_CLONE         1012
#define IDC_CLEAR_INTERNAL      1013
#define IDC_CLEAR_EXTERNAL      1014

#define IDC_MODE_FIRST          1030
#define IDC_MODE_EXTEND         1030
#define IDC_MODE_CLONE          1031
#define IDC_MODE_INTERNAL       1032
#define IDC_MODE_EXTERNAL       1033

#define IDC_SERVICE_STATUS      1020
#define IDC_MESSAGE             1021    // 入力の確認結果や保存の結果。警告は赤字
#define IDC_APPLY               1022
#define IDC_INSTALL             1023    // インストール / 再インストール
#define IDC_UNINSTALL           1024
#define IDC_VERSION             1025    // この設定画面の版とインストール済みの版
#define IDC_RESET_DEFAULTS      1026    // 入力欄を既定の割り当て（Ctrl+Alt+F9〜F12）に戻す
