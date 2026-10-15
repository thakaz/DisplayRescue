#pragma once

#include <windows.h>
#include <d3d11.h>
#include <DirectXMath.h>
#include <wil/com.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mascot/GltfMesh.h"
#include "mascot/Motion.h"
#include "mascot/Skeleton.h"
#include "mascot/SpringSimulator.h"

namespace rescue::mascot {
	// glTF（.glb / .vrm）のモデルを読み込んで、骨のアニメーションで動かし、テクスチャと陰影をつけて描く。
	//   コンストラクタで読み込んだもの（MeshData）を GPU に渡し、Draw で描く。読むのは呼ぶ側
	//   （マスコットは変換した形式を LoadModelAsset で。glTF の解析と画像を画素に戻すのは変換のときだけ）。
	//   陰影は MToon（アニメ調）。MToon の無い材質（VRM でない glTF）も、同じシェーダーでなだらかな陰影にする
	class Model {
	public:
		// mesh: 読み込んだモデル（変換した形式なら LoadModelAsset、glTF なら LoadGltfMesh）。GPU に送る。失敗は例外
		Model(ID3D11Device* device, MeshData mesh);

		// 描く。world: モデルをどう置くか（回転など）。viewProjection: カメラ（view × projection）。
		// cameraPosition: カメラの位置（縁の光に使う）。seconds: アニメーションのどの時刻の姿勢で描くか（長さを超えたら繰り返す）
		// 描く先・ビューポートは呼ぶ側（Canvas::Begin）で付けておく
		void Draw(ID3D11DeviceContext* context, DirectX::FXMMATRIX world, DirectX::CXMMATRIX viewProjection,
			const DirectX::XMFLOAT3& cameraPosition, float seconds);

		// 読み込んだモデルのあらまし（ログ用）
		const std::wstring& Info() const noexcept { return m_info; }

		// 基本の姿勢を囲む球の半径（大きさをそろえた後。モデルの背の高さがだいたい 2）
		float Radius() const noexcept { return m_radius; }

		// 名前の表情（blink / happy / aa など）の効き具合を 0〜1 で決める。次の Draw で頂点を動かす。
		// 戻り値は、その名前の表情があったか（無ければ何もしない）
		bool SetExpression(std::wstring_view name, float weight);

		// モーションを手持ちに足す（再生はしない）。番号は足した順に 0, 1, 2 …。
		// モデルが VRM でなければ例外。戻り値はあらまし（ログ用）。
		// 先に全部読んでおくのは、長さが分からないと「今どのモーションの何秒目か」を決められないため（MotionSchedule）
		// motion は呼ぶ側が読んだもの（LoadMotionAsset か LoadGltfMotion）。name はログ用
		std::wstring AddMotion(MotionData motion, std::wstring_view name);
		float MotionDuration(std::size_t index) const { return m_motions.at(index).Duration(); }

		// index 番のモーションを再生する（長さを超えたら繰り返す）。
		// fadeSeconds 秒かけて、今の姿勢（前のモーション）から移る。
		// 前のモーションをまだ見せていなければ（起動して最初の 1 本）、つながずにいきなり始める（基本の姿勢の T ポーズを見せない）。
		// start: 再生を始めた時刻（Draw の seconds と同じ物差し）。過去を渡すと途中から再生する。無ければ次の Draw の時刻
		void PlayMotion(std::size_t index, float fadeSeconds = 0.5f, std::optional<float> start = std::nullopt);
		// 再生中のモーションの長さ（秒）。無ければ 0
		float MotionDuration() const { return m_motion ? m_motions[m_motion->index].Duration() : 0.0f; }

	private:
		// 部分ごとの定数バッファ（b2）の中身。並びは HLSL の cbuffer Material と同じ。
		// HLSL の定数バッファは 16 バイトの行に詰めるので、float3 の後ろに float を 1 つ置いて行をそろえる
		struct MaterialConstants {
			float alphaFactor;			// 材質の基本色の透明度
			float alphaCutoff;			// MASK のときの境目
			std::uint32_t alphaMode;	// AlphaMode の値
			std::uint32_t isOutline;	// 1 なら輪郭線を描いているところ（Draw が描くたびに決める）
			DirectX::XMFLOAT3 shadeColor;
			float shadingShift;
			DirectX::XMFLOAT3 matcapColor;
			float shadingToony;
			DirectX::XMFLOAT3 rimColor;
			float rimFresnelPower;
			DirectX::XMFLOAT3 outlineColor;
			float rimLift;
			DirectX::XMFLOAT3 emissiveColor;
			float outlineWidth;			// 0 なら輪郭線なし
			float shadingShiftTextureScale;
			DirectX::XMFLOAT3 padding;
		};
		static_assert(sizeof(MaterialConstants) == 16 * 7);

		// 部分 1 つが使うテクスチャ（m_textures の添字。0 は白 1 画素）。並びはシェーダーの t0〜t5、最後の 1 つは頂点シェーダーの t6
		enum TextureSlot : std::size_t { BaseColor, ShadeMultiply, ShadingShift, Matcap, RimMultiply, Emissive, OutlineWidth, TextureSlotCount };

		// 描く単位。三角形の範囲と、そこに貼るテクスチャと材質
		struct Part {
			UINT firstIndex;
			UINT indexCount;
			AlphaMode alphaMode;
			bool doubleSided;
			bool hasOutline;			// 輪郭線を描くか
			int renderQueueOffset;		// 半透明どうしの順番
			std::array<std::size_t, TextureSlotCount> textures;
			MaterialConstants constants;	// b2 に送る値（isOutline だけは描くたびに決める）
		};

		// 画像 1 枚を、色として読む口（sRGB → リニアに戻す）と、値として読む口（そのまま）の 2 つで持つ。
		// 影の境目や輪郭線の太さの画像は「色」ではなく「数」なので、戻してはいけない
		struct TextureViews {
			wil::com_ptr<ID3D11ShaderResourceView> color;
			wil::com_ptr<ID3D11ShaderResourceView> value;
		};

		void DrawPart(ID3D11DeviceContext* context, const Part& part, bool outline);	// 部分 1 つを、材質に合わせた道具で描く（outline なら輪郭線）
		void ApplyMorphs(ID3D11DeviceContext* context);	// 表情の効き具合で頂点を動かし、頂点バッファに書き直す

		// モーフで動かすため、元の頂点を CPU 側にも持っておく（毎回ここから計算し直す）
		std::vector<MeshVertex> m_baseVertices;
		std::vector<MeshVertex> m_morphedVertices;	// 動かした結果（毎回メモリを取り直さないように持っておく）
		std::vector<MorphTarget> m_morphTargets;
		std::vector<MeshExpression> m_expressions;
		std::vector<float> m_expressionWeights;		// m_expressions と同じ並び。0〜1（SetExpression で決めたもの）
		// モーションが動かしている表情（MotionPlayer::AddExpressions の weights と control）。m_expressions と同じ並び
		std::vector<float> m_motionExpressionWeights;
		std::vector<float> m_motionExpressionControl;
		bool m_morphDirty = false;					// 効き具合が変わって、まだ頂点に反映していない

		std::wstring m_info;
		float m_radius = 1.0f;
		std::vector<Part> m_parts;
		std::vector<std::size_t> m_blendOrder;	// 半透明の部分（m_parts の添字）を描く順番
		Skeleton m_skeleton;	// 骨とアニメーション。毎フレーム骨の行列を計算する
		SpringSimulator m_springs;	// 髪・スカートなどを揺らす。前のフレームのテールを覚えている
		std::optional<float> m_lastSeconds;	// 前の Draw の時刻（経過時間を出すため）。最初の Draw の前は空
		HumanBones m_humanBones;				// モデルの人型の骨
		std::vector<MotionPlayer> m_motions;		// 手持ちのモーション（AddMotion の順）
		// 再生中のモーション 1 本（m_motions の番号）と、再生を始めた Draw の時刻（空なら次の Draw で決める）
		struct PlayingMotion {
			std::size_t index;
			std::optional<float> start;
		};
		std::optional<PlayingMotion> m_motion;		// 再生中のモーション。無ければモデル自身のアニメーション
		std::optional<PlayingMotion> m_fadingOut;	// クロスフェード中に表示する直前のモーション（最後の姿勢で停止し、繰り返さない）
		float m_fadeSeconds = 0.0f;					// クロスフェード時間（秒）
		std::optional<float> m_fadeStart;			// クロスフェードを開始した Draw の時刻（空なら次の Draw で決める）

		wil::com_ptr<ID3D11VertexShader> m_vertexShader;
		wil::com_ptr<ID3D11PixelShader> m_pixelShader;
		wil::com_ptr<ID3D11InputLayout> m_inputLayout;
		wil::com_ptr<ID3D11Buffer> m_vertexBuffer;
		wil::com_ptr<ID3D11Buffer> m_indexBuffer;
		wil::com_ptr<ID3D11Buffer> m_constantBuffer;
		wil::com_ptr<ID3D11Buffer> m_jointBuffer;	// 骨の行列（b1）
		wil::com_ptr<ID3D11Buffer> m_materialBuffer;	// 部分ごとの材質（b2）

		// 描き方の道具。[0] は表だけ描く、[1] は裏も描く（doubleSided）、[2]は裏だけ描く（輪郭線）
		std::array<wil::com_ptr<ID3D11RasterizerState>, 3> m_rasterizers;
		wil::com_ptr<ID3D11BlendState> m_blend;					// 半透明: 後ろの色と混ぜる
		wil::com_ptr<ID3D11DepthStencilState> m_depthReadOnly;	// 半透明: 奥行きを見るが書かない

		// 0 番は白 1 画素（テクスチャの無いところ用。白を掛けても色は変わらない）。1 番からは画像の順
		std::vector<TextureViews> m_textures;
		wil::com_ptr<ID3D11SamplerState> m_sampler;
	};
}
