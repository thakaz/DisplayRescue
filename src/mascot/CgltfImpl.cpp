// cgltf の実装を作るファイル。
// cgltf はヘッダー 1 つのライブラリで、普段 include すると宣言だけが入る。
// どこか 1 つの .cpp でだけ CGLTF_IMPLEMENTATION を定義してから include すると、そこに関数の中身が入る（2 つ以上だと重複でリンクエラー）。
// 自分のコードではないので、警告は全部止める（/sdl だと fopen などの「安全でない関数」の警告がエラーになるため、_CRT_SECURE_NO_WARNINGS も要る）。
// _CRT_SECURE_NO_WARNINGS は CRT のヘッダーより前に定義しないと効かないので、このファイルの先頭に置く
#define _CRT_SECURE_NO_WARNINGS

#pragma warning(push, 0)
#define CGLTF_IMPLEMENTATION
#include "cgltf/cgltf.h"
#pragma warning(pop)
