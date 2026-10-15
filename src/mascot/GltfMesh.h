#pragma once

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "mascot/Humanoid.h"
#include "mascot/ImageDecode.h"
#include "mascot/MToon.h"
#include "mascot/Motion.h"
#include "mascot/Skeleton.h"
#include "mascot/SpringBone.h"

namespace rescue::mascot {
	// 骨の行列（パレット）の最大数。シェーダーの cbuffer Joints の配列の大きさと同じ（Model.cpp）。
	// 1024 は定数バッファの上限（64 KB = float4x4 の 1024 個）。VRM はスキンが複数あり、合計で 360 本ほどになる
	constexpr std::size_t kMaxJoints = 1024;

	// 頂点 1 つ分。並びは Model.cpp の入力レイアウト（kInputElements）と HLSL の VertexIn に合わせる
	struct MeshVertex {
		DirectX::XMFLOAT3 position;	// メッシュの座標のまま（骨の行列はシェーダーで掛ける）
		DirectX::XMFLOAT3 normal;
		DirectX::XMFLOAT3 color;
		DirectX::XMFLOAT2 uv;		// テクスチャのどこを貼るか（0〜1。左上が 0,0）
		DirectX::XMUINT4 joints;	// どの骨の行列で動かすか（Skeleton::palette の番号 4 つ）
		DirectX::XMFLOAT4 weights;	// 4 本の骨をどれだけの割合で混ぜるか（足して 1）
	};

	// 材質の透明の扱い（glTF の alphaMode）
	enum class AlphaMode : std::uint32_t {
		Opaque = 0,	// 不透明。テクスチャの透明度は無視する
		Mask = 1,	// 切り抜き。透明度が alphaCutoff より小さいピクセルは描かない（まつ毛・髪の先など）
		Blend = 2,	// 半透明。後ろの色と混ぜる（目のハイライト・眉など）。不透明のものを全部描いてから描く
	};

	// 三角形の番号（indices）の一続きの範囲と、そこに貼る画像。
	// 材質（テクスチャ）ごとに分けて描くため。glTF のプリミティブ 1 つが 1 つになる
	struct MeshPart {
		std::uint32_t firstIndex = 0;			// indices の何番目から
		std::uint32_t indexCount = 0;			// いくつ（3 の倍数）
		std::optional<std::size_t> image;		// images の何番目を貼るか。無ければ材質の色だけ
		AlphaMode alphaMode = AlphaMode::Opaque;	// 透明の扱い
		float alphaCutoff = 0.5f;				// Mask のときの境目（glTF の既定は 0.5）
		float alpha = 1.0f;						// 材質の基本色の透明度（テクスチャの透明度に掛ける）
		bool doubleSided = false;				// 裏側も描くか（髪・服の薄い板は裏からも見える）
		DirectX::XMFLOAT3 emissive{ 0, 0, 0 };	// 自分で光る色（glTF の emissiveFactor）
		std::optional<std::size_t> emissiveImage;	// emissive に掛ける画像（images の番号）
		// VRM の MToon の値。材質に VRMC_materials_mtoon が無ければ空（Model が普通の陰影で描く）。
		// テクスチャの番号は、GltfMesh.cpp が glTF の textures の番号から images の番号に置き換えてある
		std::optional<MToonMaterial> mtoon;
	};

	// モーフで頂点 1 つをずらす量。vertex は MeshData::vertices の番号（ずれない頂点は持たない）
	struct MorphDelta {
		std::uint32_t vertex = 0;
		DirectX::XMFLOAT3 position{ 0, 0, 0 };	// 位置をずらす量（メッシュの座標）
		DirectX::XMFLOAT3 normal{ 0, 0, 0 };	// 法線をずらす量
	};

	// モーフ 1 つ（glTF のプリミティブ 1 つの targets の 1 つ）。効き具合 1 のとき、各頂点を delta だけずらす
	struct MorphTarget {
		std::vector<MorphDelta> deltas;
	};

	// 表情 1 つ。名前と、動かすモーフ（MeshData::morphTargets の番号）と割合。
	// VRM の表情はノードとモーフの番号で書いてあるが、GltfMesh.cpp が morphTargets の番号に置き換えてある
	// （メッシュが複数のプリミティブに分かれていると、1 つのモーフの番号がプリミティブの数だけの MorphTarget になる）
	struct MeshExpression {
		struct Morph {
			std::size_t target = 0;
			float weight = 1.0f;
		};
		std::wstring name;
		bool isBinary = false;
		std::vector<Morph> morphs;
	};

	// 描ける形にしたモデル。頂点はメッシュの座標で、骨の行列（skeleton.JointMatrices）を掛けると
	// D3D の座標（左手系）で全体が -1〜1 に収まる
	struct MeshData {
		std::vector<MeshVertex> vertices;
		std::vector<std::uint32_t> indices;	// 3 つで三角形 1 つ。表は時計回り
		std::vector<MeshPart> parts;		// indices の範囲ごとの貼る画像
		std::vector<DecodedImage> images;	// 画像を画素に戻したもの。読めなかった画像は空（幅 0）
		Skeleton skeleton;					// 骨とアニメーション
		std::vector<MorphTarget> morphTargets;	// モーフ
		std::vector<MeshExpression> expressions;	// VRM の表情（VRM でなければ空）
		SpringBoneData springBones;			// VRM の揺れもの（VRM でなければ空）
		HumanBones humanBones;				// 人型の骨の名前 → ノードの番号（VRM でなければ空）
		std::wstring info;					// あらまし（ログ用）
		float radius = 1.0f;				// 大きさをそろえた（normalize）後の、原点（箱の中心）から箱の角までの距離。どう回してもこの球に入る
	};

	// glTF（.glb）を読んで MeshData にする。GPU には触らない（GPU に渡すのは Model）。
	// アニメーションは 1 つ目だけ、移動・回転・拡大だけ（アニメーションでモーフを動かすのは未対応）。テクスチャは基本色・自分で光る色と、MToon のもの（法線マップは未対応）。
	// 外部ファイルを参照する .gltf は未対応。失敗は例外
	MeshData LoadGltfMesh(const std::filesystem::path& path);

	// .vrma（VRM のモーション。中身は glTF）を読む。骨とアニメーション（1 つ目）と人型の骨。失敗は例外
	MotionData LoadGltfMotion(const std::filesystem::path& path);
}
