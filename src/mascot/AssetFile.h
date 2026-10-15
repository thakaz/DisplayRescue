#pragma once

#include <filesystem>

#include "mascot/GltfMesh.h"
#include "mascot/Motion.h"

namespace rescue::mascot {
	// マスコットが読む、自前の単純な形式。
	// ログイン画面の SYSTEM が、他人の作ったファイル（glTF・JSON・PNG / JPEG）を cgltf や WIC で解析しないようにするため。
	// 解析は設定アプリが取り込むときに、変換（DisplayRescueMascot.exe --convert-model / --convert-motion）で済ませ、
	// 結果（MeshData / MotionData）をそのまま並べて書く。マスコットはこの形式だけを読む。
	//
	// 形式: 先頭に印（8 バイト、config::kMascotModelMagic / kMascotMotionMagic）と版（4 バイト、config::kMascotAssetVersion）。
	// あとは決まった順に、数（8 バイト）と中身を並べただけ。数字はどれもリトルエンディアン（x64 のメモリのまま）。
	// 読むときは、数を読むたびに「残りのバイト数で足りるか」を確かめ（足りなければ例外）、最後に番号の範囲を全部確かめる（Validate*）。
	// 版が違えば HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH)、壊れていれば HRESULT_FROM_WIN32(ERROR_INVALID_DATA) の例外

	// 書く。いったん path + ".tmp" に書き、書き終えてから置き換える（途中で失敗しても前のファイルは壊れない）
	void SaveModelAsset(const MeshData& mesh, const std::filesystem::path& path);
	void SaveMotionAsset(const MotionData& motion, const std::filesystem::path& path);

	// 読む（中身の番号の範囲も確かめる）
	MeshData LoadModelAsset(const std::filesystem::path& path);
	MotionData LoadMotionAsset(const std::filesystem::path& path);

	// 番号の範囲を確かめる（Model や Skeleton が添字に使うものが、配列の外を指していないか）。だめなら ERROR_INVALID_DATA の例外。
	// 読むときのほか、変換のときにも呼ぶ（cgltf が読めても、こちらの決まりに合わないものは取り込まない）
	void ValidateMesh(const MeshData& mesh);
	void ValidateMotion(const MotionData& motion);
}
