// 読み込み（GltfMesh.cpp か変換した形式）が作った頂点と三角形と画素を GPU に渡し、MToon の陰影と輪郭線をつけて描く。
//   シェーダー: VSMain は骨の行列（スキニング）と輪郭線の押し出し、PSMain は透明・MToon の影・縁の光
//   コンストラクタ: (2)シェーダー (3)バッファ（モーフのあるモデルは頂点を DYNAMIC に） (4)テクスチャ (5)部分ごとの材質 (6)描き方の道具
//   Draw: (1)骨の行列（モーション → クロスフェード → 揺れもの → パレット）を送り、不透明 → 輪郭線 → 半透明の順に部分ごとに描く
//   表情（モーフ）は ApplyMorphs で頂点を CPU で動かして書き直す（Map / Unmap）
#include "mascot/Model.h"

#include <d3dcompiler.h>
#include <wil/result.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "mascot/GltfMesh.h"
#include "mascot/ImageDecode.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace {
	using namespace DirectX;
	using rescue::mascot::MeshVertex;

	// シェーダー。配列の大きさ 1024 は kMaxJoints（GltfMesh.h）と同じ
	constexpr std::string_view kShaderSource = R"(
cbuffer Scene : register(b0)
{
	float4x4 worldViewProjection;
	float4x4 world;		// 法線を回すため（モデルの置き方だけ。カメラは含めない）
	float3 cameraPosition;	// カメラの位置（縁の光で、ピクセルからカメラへの向きを出すため）
	float  scenePadding;
};

// 骨の行列（パレット）。頂点の joints がこの番号を指す。毎フレーム C++ 側（Draw）で送る
cbuffer Joints : register(b1)
{
	float4x4 joints[1024];
};

// 部分ごとの材質。並びは C++ の Model::MaterialConstants（Model.h）と同じ。alphaMode の値は AlphaMode（GltfMesh.h）と同じ
cbuffer Material : register(b2)
{
	float  alphaFactor;		// 材質の基本色の透明度
	float  alphaCutoff;		// MASK のときの境目
	uint   alphaMode;
	uint   isOutline;		// 1 なら輪郭線を描いている
	float3 shadeColor;		// 影の側の色
	float  shadingShift;	// 影の境目をずらす量
	float3 matcapColor;
	float  shadingToony;	// 影の境目のくっきり具合（0 でなだらか、1 で 2 段）
	float3 rimColor;
	float  rimFresnelPower;
	float3 outlineColor;
	float  rimLift;
	float3 emissiveColor;
	float  outlineWidth;	// 輪郭線の太さ（モデルの座標。0 なら輪郭線なし）
	float  shadingShiftTextureScale;
	float3 padding;
};
static const uint ALPHA_OPAQUE = 0;
static const uint ALPHA_MASK = 1;
static const uint ALPHA_BLEND = 2;

// t0〜t5 はピクセルシェーダー、t6 は頂点シェーダーが読む。s0 はサンプラー（テクスチャの読み方）。C++ 側（DrawPart）で付ける
Texture2D baseColorTexture : register(t0);
Texture2D shadeMultiplyTexture : register(t1);	// 影の色に掛ける
Texture2D shadingShiftTexture : register(t2);	// 影の境目をずらす（R）
Texture2D matcapTexture : register(t3);			// 法線の向きで引く光の画像
Texture2D rimMultiplyTexture : register(t4);	// 縁の光に掛ける
Texture2D emissiveTexture : register(t5);		// 自分で光る色に掛ける
Texture2D outlineWidthTexture : register(t6);	// 輪郭線の太さに掛ける（G）
SamplerState linearSampler : register(s0);

struct VertexIn
{
	float3 position : POSITION;
	float3 normal   : NORMAL;
	float3 color    : COLOR;
	float2 uv       : TEXCOORD;
	uint4  jointIds : BLENDINDICES;	// どの骨か（4 本）
	float4 weights  : BLENDWEIGHT;	// それぞれの割合（足して 1）
};

struct VertexOut
{
	float4 position : SV_POSITION;
	float3 normal   : NORMAL;
	float3 color    : COLOR;
	float2 uv       : TEXCOORD;
	float3 worldPosition : TEXCOORD1;	// 置いたあとの位置（名前は TEXCOORD1。シェーダーの間で渡す値の名前は何でもよい）
};

// a から b の間で 0 → 1 にまっすぐ上がり、外は 0 か 1 に止める（MToon の影の境目に使う）
float linearstep(float a, float b, float t)
{
	return saturate((t - a) / (b - a));
}

VertexOut VSMain(VertexIn input)
{
	// この頂点を動かす行列。関節の近くの頂点は、2〜4 本の骨に少しずつ付いている。4 本の行列を weights の割合で混ぜる
	const float4x4 skin =
	joints[input.jointIds.x] * input.weights.x +
	joints[input.jointIds.y] * input.weights.y +
	joints[input.jointIds.z] * input.weights.z +
	joints[input.jointIds.w] * input.weights.w;

	// 輪郭線: モデルを法線の向きに少し太らせて、裏側だけを輪郭線の色で描く（C++ 側で表を捨てる道具を付ける）。
	// 太らせた裏側は、元の体の縁からはみ出したところだけが見えて、それが線になる。太さは骨で動かす前（メッシュの座標）で足す
	float3 position = input.position;
	if (isOutline != 0)
	{
		const float width = outlineWidth * outlineWidthTexture.SampleLevel(linearSampler, input.uv, 0).g;
		// 頂点を法線の向きに width だけ押し出す
		position += normalize(input.normal) * width;
	}

	// 頂点を骨の行列で動かしてから（モデルの座標になる）、world と viewProjection を掛ける。法線も同じ行列で回す
	const float4 skinnedPosition = mul(float4(position, 1.0), skin);
	const float4 skinnedNormal = mul(float4(input.normal, 0.0), skin);

	VertexOut output;
	output.position = mul(skinnedPosition, worldViewProjection);
	output.normal = mul(skinnedNormal, world).xyz;	// w = 0 にすると平行移動を受けず、向きだけ回る
	output.color = input.color;
	output.uv = input.uv;	// 三角形の中のピクセルには、3 つの頂点の UV を混ぜた値が届く
	output.worldPosition = mul(skinnedPosition, world).xyz;
	return output;
}

// isFrontFace: このピクセルが三角形の表か裏か（裏も描く部分で、法線を裏返すため）
float4 PSMain(VertexOut input, bool isFrontFace : SV_IsFrontFace) : SV_TARGET
{
	// このピクセルの色: テクスチャの UV の位置の色（Sample は近くの画素を混ぜて返す）× 材質の色
	const float4 texel = baseColorTexture.Sample(linearSampler, input.uv);
	const float3 baseColor = texel.rgb * input.color;

	// 透明度: テクスチャの透明度 × 材質の透明度
	float alpha = texel.a * alphaFactor;
	// MASK の部分は、透明度が境目（alphaCutoff）より小さいピクセルを描かない。clip は引数が負ならそのピクセルを捨てる
	if (alphaMode == ALPHA_MASK) clip(alpha - alphaCutoff);

	if (alphaMode != ALPHA_BLEND) alpha = 1.0;	// OPAQUE と MASK は、描いたピクセルは不透明

	// 輪郭線は、輪郭線の色だけで塗る（切り抜きは上で済んでいる）
	if (isOutline != 0) return float4(outlineColor, alpha);

	// 裏から見ているピクセルは、法線も裏を向かせる（そうしないと光の当たり方が逆になる）
	const float3 normal = normalize(input.normal) * (isFrontFace ? 1.0 : -1.0);
	const float3 toLight = normalize(float3(-0.4, 0.8, -0.6));		// 光の来る方向（左上の手前）

	// 1. MToon の影の濃さ（shading）: 光の向きとの内積（-1〜1。光を向いているほど大きい）を、境目をずらしてから 0〜1 にする。
	// 1 なら明るい側（基本色）、0 なら影の側（影の色）
	float shading = dot(normal, toLight);
	shading += shadingShift + shadingShiftTexture.Sample(linearSampler, input.uv).r * shadingShiftTextureScale;
	// 影の境目をくっきりさせる。shadingToony が 0 なら -1〜1 をなだらかに、1 に近いほど境目の前後だけで 0 → 1 に変わる
	shading = linearstep(-1.0 + shadingToony, 1.0 - shadingToony, shading);

	// 2. 明るい側と影の側を shading で混ぜる。影は「暗くする」のではなく、モデルが指定した影の色に置き換える
	const float3 shade = shadeColor * shadeMultiplyTexture.Sample(linearSampler, input.uv).rgb;
	float3 color = lerp(shade, baseColor, shading);

	// 3. 縁の光（リムライト）。マットキャップ: 法線の向き（x, y）をそのまま画像の位置にして、光の当たり方を描いた画像から引く。
	// パラメトリック: カメラに対して横を向いた面（縁）ほど明るくする。カメラへの向きはピクセルごとに出す（画面の端では正面とずれるため）。
	// 法線は頂点の値を混ぜたものなので、カメラから見えている面でも、ところどころカメラと反対を向く（服のひだなど）。
	// そこで 1 − dot が 1 を超えて縁の光が最大になり、白い点が散った。カメラを向いていない法線では縁の光を足さない
	const float2 matcapUv = normal.xy * float2(0.5, -0.5) + 0.5;
	float3 rim = matcapColor * matcapTexture.Sample(linearSampler, matcapUv).rgb;
	const float facing = dot(normal, normalize(cameraPosition - input.worldPosition));
	if (facing > 0.0) rim += rimColor * pow(saturate(1.0 - facing + rimLift), rimFresnelPower);
	color += rim * rimMultiplyTexture.Sample(linearSampler, input.uv).rgb;

	// 4. 自分で光る色
	color += emissiveColor * emissiveTexture.Sample(linearSampler, input.uv).rgb;
	return float4(color, alpha);
}
)";

	// 頂点の並び（MeshVertex と同じ）。BLENDINDICES / BLENDWEIGHT はスキニング（どの骨か・その割合）。
	// 何バイト目からかは offsetof（構造体の中の位置）で書く。手で数えるとずれたときに気づけない
	constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 6> kInputElements{ {
		{ "POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(MeshVertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(MeshVertex, normal),   D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR",        0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(MeshVertex, color),    D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(MeshVertex, uv),       D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT,  0, offsetof(MeshVertex, joints),   D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(MeshVertex, weights),  D3D11_INPUT_PER_VERTEX_DATA, 0 },
	} };

	// 骨の行列の定数バッファの大きさ。並びは HLSL の cbuffer Joints と同じ（64 KB）
	constexpr UINT kJointBufferBytes = sizeof(XMFLOAT4X4) * rescue::mascot::kMaxJoints;

	// 定数バッファの中身。並びは HLSL の cbuffer Scene と同じ
	struct SceneConstants {
		XMFLOAT4X4 worldViewProjection;
		XMFLOAT4X4 world;
		XMFLOAT3 cameraPosition;
		float padding;
	};

	// HLSL をコンパイルする。失敗したらコンパイラのメッセージ付きで例外
	wil::com_ptr<ID3DBlob> CompileShader(const char* entryPoint, const char* target) {
		wil::com_ptr<ID3DBlob> code;
		wil::com_ptr<ID3DBlob> errors;
		const HRESULT hr = D3DCompile(kShaderSource.data(), kShaderSource.size(), nullptr, nullptr, nullptr,
			entryPoint, target, 0, 0, code.put(), errors.put());
		THROW_IF_FAILED_MSG(hr, "%hs", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
		return code;
	}

	// 画素（R G B A 各 1 バイト、上の行から）から GPU のテクスチャを作り、シェーダーから読む口（SRV）を返す。
	// 縮小版（ミップマップ: 1/2, 1/4, … の大きさの画像）も GPU に作らせる。モデルは小さく表示するので、
	// 縮小版が無いと 1 ピクセルに大きな画像の 1 点だけを拾うことになり、回すとちらつく。
	// 画像の色は sRGB（画面に出す用に明るさを曲げた値）で保存されている。
	// _SRGB の形式で読むと、シェーダーが読むときに「光の量」（リニア）に戻してくれる。陰影の計算はリニアで行い、
	// 画用紙（Canvas）の sRGB の口が書き込むときに画面用に戻す。
	// 同じ画像を「色」（_SRGB で戻す）と「数」（_UNORM でそのまま）の両方で読めるように、
	// テクスチャは型なし（TYPELESS）で作り、口を 2 つ作る。影の境目や輪郭線の太さの画像は数なので、戻してはいけない
	struct CreatedTexture {
		wil::com_ptr<ID3D11ShaderResourceView> color;
		wil::com_ptr<ID3D11ShaderResourceView> value;
	};
	CreatedTexture CreateTexture(ID3D11Device* device, UINT width, UINT height, std::span<const std::byte> pixels) {
		// MipLevels = 0 は「1 画素になるまで全部」。GenerateMips を使うには、描く先（RENDER_TARGET）にもなれる必要がある
		const D3D11_TEXTURE2D_DESC desc{
			.Width = width, .Height = height, .MipLevels = 0, .ArraySize = 1,
			.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS, .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, .MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS };
		wil::com_ptr<ID3D11Texture2D> texture;
		THROW_IF_FAILED(device->CreateTexture2D(&desc, nullptr, texture.put()));

		// 画像の色をどう読むか。_SRGB にすると、読むときに sRGB → リニアに戻る（画用紙が書き込むときにまた sRGB にする）。
		// テクスチャ自体は型なしにしたので、形式は口のほうで決める。MipLevels = -1 は「全部の段」
		D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
		viewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		viewDesc.Texture2D.MipLevels = static_cast<UINT>(-1);
		CreatedTexture views;
		THROW_IF_FAILED(device->CreateShaderResourceView(texture.get(), &viewDesc, views.color.put()));
		viewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		THROW_IF_FAILED(device->CreateShaderResourceView(texture.get(), &viewDesc, views.value.put()));

		// いちばん大きい画像（0 段目）だけ書き込んで、残りの段は GPU に縮小して作ってもらう。行の長さは幅 × 4 バイト。
		// 縮小は色の口で頼む（明るさをリニアに戻してから混ぜるので、縮小版が暗くならない）
		wil::com_ptr<ID3D11DeviceContext> context;
		device->GetImmediateContext(context.put());
		context->UpdateSubresource(texture.get(), 0, nullptr, pixels.data(), width * 4, 0);
		context->GenerateMips(views.color.get());
		return views;
	}

	// 画像の番号（MeshData::images の添字）を、m_textures の添字にする（0 番は白なので 1 つずれる）。無ければ白
	std::size_t TextureIndex(const std::optional<std::size_t>& image) {
		return image ? *image + 1 : 0;
	}
}

namespace rescue::mascot {

	Model::Model(ID3D11Device* device, MeshData mesh) {
		// 1. 読み込み。ファイルを読むのは呼ぶ側（変換した形式なら LoadModelAsset、glTF なら LoadGltfMesh）。
		// ここは受け取ったものを GPU に送るだけ
		m_info = mesh.info;
		m_radius = mesh.radius;
		m_skeleton = std::move(mesh.skeleton);
		m_springs = SpringSimulator(mesh.springBones, m_skeleton.parents.size());
		m_humanBones = std::move(mesh.humanBones);

		// 2. シェーダーと入力レイアウト
		const auto vertexCode = CompileShader("VSMain", "vs_5_0");
		const auto pixelCode = CompileShader("PSMain", "ps_5_0");
		THROW_IF_FAILED(device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr, m_vertexShader.put()));
		THROW_IF_FAILED(device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr, m_pixelShader.put()));
		THROW_IF_FAILED(device->CreateInputLayout(kInputElements.data(), static_cast<UINT>(kInputElements.size()),
			vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), m_inputLayout.put()));

		// 3. バッファ。モーフのあるモデルは、頂点を CPU で動かして書き直すので、頂点バッファを DYNAMIC（CPU から書ける）にする
		const bool morphing = !mesh.morphTargets.empty();
		const D3D11_BUFFER_DESC vertexDesc{
			.ByteWidth = static_cast<UINT>(mesh.vertices.size() * sizeof(MeshVertex)),
			.Usage = morphing ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_IMMUTABLE,
			.BindFlags = D3D11_BIND_VERTEX_BUFFER,
			.CPUAccessFlags = morphing ? D3D11_CPU_ACCESS_WRITE : 0u };
		const D3D11_SUBRESOURCE_DATA vertexData{ .pSysMem = mesh.vertices.data() };
		THROW_IF_FAILED(device->CreateBuffer(&vertexDesc, &vertexData, m_vertexBuffer.put()));
		if (morphing) {
			m_baseVertices = mesh.vertices;
			m_morphTargets = std::move(mesh.morphTargets);
			m_expressions = std::move(mesh.expressions);
			m_expressionWeights.assign(m_expressions.size(), 0.0f);
			m_motionExpressionWeights.assign(m_expressions.size(), 0.0f);
			m_motionExpressionControl.assign(m_expressions.size(), 0.0f);
		}

		const D3D11_BUFFER_DESC indexDesc{ .ByteWidth = static_cast<UINT>(mesh.indices.size() * sizeof(std::uint32_t)), .Usage = D3D11_USAGE_IMMUTABLE, .BindFlags = D3D11_BIND_INDEX_BUFFER };
		const D3D11_SUBRESOURCE_DATA indexData{ .pSysMem = mesh.indices.data() };
		THROW_IF_FAILED(device->CreateBuffer(&indexDesc, &indexData, m_indexBuffer.put()));

		const D3D11_BUFFER_DESC constantDesc{ .ByteWidth = sizeof(SceneConstants), .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_CONSTANT_BUFFER };
		THROW_IF_FAILED(device->CreateBuffer(&constantDesc, nullptr, m_constantBuffer.put()));

		// 骨の行列。中身は毎フレーム Draw で送る
		const D3D11_BUFFER_DESC jointDesc{ .ByteWidth = kJointBufferBytes, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_CONSTANT_BUFFER };
		THROW_IF_FAILED(device->CreateBuffer(&jointDesc, nullptr, m_jointBuffer.put()));

		// 4. 画素を GPU のテクスチャにする。
		// 0 番は白 1 画素。読めない画像（中身が空）も白にして、番号がずれないようにする
		constexpr std::array<std::byte, 4> kWhite{ std::byte{ 255 }, std::byte{ 255 }, std::byte{ 255 }, std::byte{ 255 } };
		const auto addTexture = [&](UINT width, UINT height, std::span<const std::byte> pixels) {
			CreatedTexture created = CreateTexture(device, width, height, pixels);
			m_textures.push_back({ std::move(created.color), std::move(created.value) });
		};
		addTexture(1, 1, kWhite);
		// 画素には戻してある（GltfMesh.cpp か、変換した形式）
		for (const DecodedImage& image : mesh.images) {
			if (image.width == 0 || image.height == 0) {
				m_textures.push_back(m_textures.front());
				continue;
			}
			addTexture(image.width, image.height, image.pixels);
		}

		// 5. 部分ごとに、b2 に送る値と、貼るテクスチャを決める。
		// MToon のある材質はその値を写す。無い材質（VRM でない glTF）は、ふつうの陰影に近い見た目になる値にする:
		// 影の色 = 基本色 × 0.35、なだらか（shadingToony = 0）、縁の光と輪郭線なし
		for (const MeshPart& source : mesh.parts) {
			Part part{};
			part.firstIndex = source.firstIndex;
			part.indexCount = source.indexCount;
			part.alphaMode = source.alphaMode;
			part.doubleSided = source.doubleSided;

			MaterialConstants& c = part.constants;
			c.alphaFactor = source.alpha;
			c.alphaCutoff = source.alphaCutoff;
			c.alphaMode = static_cast<std::uint32_t>(source.alphaMode);
			c.emissiveColor = source.emissive;
			part.textures.fill(0);
			part.textures[BaseColor] = TextureIndex(source.image);
			part.textures[Emissive] = TextureIndex(source.emissiveImage);

			if (source.mtoon) {
				const MToonMaterial& m = *source.mtoon;
				c.shadeColor = m.shadeColor;
				c.shadingShift = m.shadingShift;
				c.shadingToony = m.shadingToony;
				c.shadingShiftTextureScale = m.shadingShiftTexture ? m.shadingShiftTextureScale : 0.0f;	// テクスチャが無ければ白を読むので、掛ける量を 0 に
				c.matcapColor = m.matcapTexture ? m.matcapColor : XMFLOAT3{ 0, 0, 0 };	// 同じく、無ければ足さない（白を足すと真っ白になる）
				c.rimColor = m.rimColor;
				c.rimFresnelPower = m.rimFresnelPower;
				c.rimLift = m.rimLift;
				c.outlineColor = m.outlineColor;
				c.outlineWidth = m.outlineWidthMode == OutlineWidthMode::None ? 0.0f : m.outlineWidth;
				part.textures[ShadeMultiply] = TextureIndex(m.shadeMultiplyTexture);
				part.textures[ShadingShift] = TextureIndex(m.shadingShiftTexture);
				part.textures[Matcap] = TextureIndex(m.matcapTexture);
				part.textures[RimMultiply] = TextureIndex(m.rimMultiplyTexture);
				part.textures[OutlineWidth] = TextureIndex(m.outlineWidthMultiplyTexture);
				part.renderQueueOffset = m.renderQueueOffset;
			}
			else {
				c.shadeColor = { 0.35f, 0.35f, 0.35f };
				c.shadingToony = 0.0f;
				part.textures[ShadeMultiply] = part.textures[BaseColor];
			}
			part.hasOutline = c.outlineWidth > 0.0f;
			m_parts.push_back(part);
		}

		// 半透明の部分を描く順番: renderQueueOffset の小さい順。同じならファイルの順（stable_sort は同じ値の並びを崩さない）
		for (std::size_t i = 0; i < m_parts.size(); ++i) {
			if (m_parts[i].alphaMode == AlphaMode::Blend) m_blendOrder.push_back(i);
		}
		std::ranges::stable_sort(m_blendOrder, {}, [&](std::size_t i) { return m_parts[i].renderQueueOffset; });

		const auto mtoonCount = std::ranges::count_if(mesh.parts, [](const MeshPart& part) { return part.mtoon.has_value(); });
		const auto outlineCount = std::ranges::count_if(m_parts, &Part::hasOutline);
		const auto maskCount = std::ranges::count(m_parts, AlphaMode::Mask, &Part::alphaMode);
		m_info += std::format(L" / 切り抜き{}個・半透明{}個 / MToon {}個・輪郭線{}個", maskCount, m_blendOrder.size(), mtoonCount, outlineCount);

		// 6. サンプラー: テクスチャの読み方。
		// LINEAR は近くの画素を混ぜて滑らかに（縮小版の段の間も混ぜる）。WRAP は UV が 0〜1 をはみ出したら繰り返す（glTF の既定）
		const D3D11_SAMPLER_DESC samplerDesc{
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_WRAP, .AddressV = D3D11_TEXTURE_ADDRESS_WRAP, .AddressW = D3D11_TEXTURE_ADDRESS_WRAP,
			.ComparisonFunc = D3D11_COMPARISON_NEVER, .MaxLOD = D3D11_FLOAT32_MAX };
		THROW_IF_FAILED(device->CreateSamplerState(&samplerDesc, m_sampler.put()));

		// 7. 描き方の道具。何も付けないときの既定は「表だけ描く・混ぜない・奥行きを見て書く」
		const D3D11_BUFFER_DESC materialDesc{ .ByteWidth = sizeof(MaterialConstants), .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_CONSTANT_BUFFER };
		THROW_IF_FAILED(device->CreateBuffer(&materialDesc, nullptr, m_materialBuffer.put()));

		// 表だけ描く（裏を向いた三角形は捨てる。表は時計回り）／裏も描く／裏だけ描く（輪郭線）。
		// MultisampleEnable は線を描くときの MSAA の指定（三角形は付けなくても MSAA になるが、意図をはっきりさせる）
		D3D11_RASTERIZER_DESC rasterizerDesc{ .FillMode = D3D11_FILL_SOLID, .CullMode = D3D11_CULL_BACK, .DepthClipEnable = TRUE, .MultisampleEnable = TRUE };
		THROW_IF_FAILED(device->CreateRasterizerState(&rasterizerDesc, m_rasterizers[0].put()));
		rasterizerDesc.CullMode = D3D11_CULL_NONE;
		THROW_IF_FAILED(device->CreateRasterizerState(&rasterizerDesc, m_rasterizers[1].put()));
		rasterizerDesc.CullMode = D3D11_CULL_FRONT;
		THROW_IF_FAILED(device->CreateRasterizerState(&rasterizerDesc, m_rasterizers[2].put()));

		// 半透明の混ぜ方: 色 = 自分 × 透明度 ＋ 後ろ × (1 − 透明度)。
		// 透明度の列は「自分 ＋ 後ろ × (1 − 自分)」。背景が透明（0）のところに描くと、画用紙には「透明度を掛けた色」が残り、
		// DirectComposition の PREMULTIPLIED（色に透明度が掛けてある前提）と合う
		D3D11_BLEND_DESC blendDesc{};
		D3D11_RENDER_TARGET_BLEND_DESC& target = blendDesc.RenderTarget[0];
		target.BlendEnable = TRUE;
		target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
		target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		target.BlendOp = D3D11_BLEND_OP_ADD;
		target.SrcBlendAlpha = D3D11_BLEND_ONE;
		target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		THROW_IF_FAILED(device->CreateBlendState(&blendDesc, m_blend.put()));

		// 半透明は奥行きを「見る」が「書かない」。書くと、後から描く半透明（目の上のハイライトなど）が後ろとみなされて消える
		const D3D11_DEPTH_STENCIL_DESC depthDesc{ .DepthEnable = TRUE, .DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO, .DepthFunc = D3D11_COMPARISON_LESS };
		THROW_IF_FAILED(device->CreateDepthStencilState(&depthDesc, m_depthReadOnly.put()));
	}

	// 1 フレーム描く。骨の行列を送り、部分ごとに材質とテクスチャを付け替えて描く
	void Model::Draw(ID3D11DeviceContext* context, FXMMATRIX world, CXMMATRIX viewProjection, const XMFLOAT3& cameraPosition, float seconds) {
		// 1. この時刻の骨の行列を計算して（Skeleton.cpp）、b1 に送る。HLSL に渡す行列は転置する（HLSL は列優先で読むため）
		//   64 KB あるのでスタックの std::array ではなく vector に置く。増やした後ろは 0 で埋まる（どの頂点も指さないので何でもよい）
		//   (1)ノードの置き方 → (2)世界の置き方 → 揺れで骨を回す → (3)パレット。揺れには、モデルを画面にどう置いたか（normalize × world）と経過時間を渡す
		const float deltaSeconds = m_lastSeconds ? seconds - *m_lastSeconds : 0.0f;
		m_lastSeconds = seconds;
		//   モーションがあれば、基本の姿勢に、モーションの人型の骨の動きを書き込む（時刻は再生を始めてから）
		//   始めてから m_fadeSeconds の間は、前の姿勢（前のモーションの続き）から混ぜる。
		//   重みは smoothstep（0〜1 を S 字に曲げる）で、動き出しと止まりぎわをゆっくりにする
		std::vector<NodeTransform> local;
		// モーションが動かす表情（AddExpressions の説明）。体と同じクロスフェードの重みで、前のモーションと混ぜる
		std::vector<float> motionWeights(m_expressions.size(), 0.0f);
		std::vector<float> motionControl(m_expressions.size(), 0.0f);
		if (m_motion) {
			if (!m_motion->start) m_motion->start = seconds;
			// 始めた時刻を外から渡されると、わずかに未来のこともあるので 0 で止める
			const float elapsed = (std::max)(seconds - *m_motion->start, 0.0f);
			const MotionPlayer& player = m_motions[m_motion->index];
			const float time = std::fmod(elapsed, (std::max)(player.Duration(), 1e-3f));
			local = m_skeleton.LocalTransforms(std::nullopt);
			player.Apply(time, m_skeleton, local);

			// クロスフェード時間は「再生を始めた時刻」ではなく「PlayMotion を呼んだ時刻」から数える。
			// スケジュールに戻るときは、以前から再生中のモーションの途中へ戻る。再生開始時刻を基準にするとクロスフェード済みと判定され、姿勢が急に切り替わってしまう
			if (!m_fadeStart) m_fadeStart = seconds;
			const float progress = m_fadeSeconds > 0.0f ? std::clamp((seconds - *m_fadeStart) / m_fadeSeconds, 0.0f, 1.0f) : 1.0f;
			const float weight = progress * progress * (3.0f - 2.0f * progress);
			player.AddExpressions(time, weight, motionWeights, motionControl);
			if (progress < 1.0f) {
				std::vector<NodeTransform> from = m_skeleton.LocalTransforms(std::nullopt);
				if (m_fadingOut && m_fadingOut->start) {
					// 前のモーションは繰り返さず、最後まで行ったら最後の姿勢で止める
					const MotionPlayer& previous = m_motions[m_fadingOut->index];
					const float previousTime = std::clamp(seconds - *m_fadingOut->start, 0.0f, previous.Duration());
					previous.Apply(previousTime, m_skeleton, from);
					previous.AddExpressions(previousTime, 1.0f - weight, motionWeights, motionControl);
				}
				BlendPose(from, local, weight);
			}
			else {
				m_fadingOut.reset();
			}
		}
		else {
			local = m_skeleton.LocalTransforms(seconds);
		}
		if (motionWeights != m_motionExpressionWeights || motionControl != m_motionExpressionControl) {
			m_motionExpressionWeights = std::move(motionWeights);
			m_motionExpressionControl = std::move(motionControl);
			m_morphDirty = true;
		}
		// 0. 効き具合が変わっていたら、頂点を動かして書き直す（モーションの表情を決めてから）
		if (m_morphDirty) ApplyMorphs(context);

		std::vector<XMFLOAT4X4> nodeWorld = m_skeleton.WorldTransforms(local);
		m_springs.Update(m_skeleton, local, nodeWorld, XMLoadFloat4x4(&m_skeleton.normalize) * world, deltaSeconds);
		std::vector<XMFLOAT4X4> joints = m_skeleton.Palette(nodeWorld);
		for (XMFLOAT4X4& m : joints) {
			XMStoreFloat4x4(&m, XMMatrixTranspose(XMLoadFloat4x4(&m)));
		}
		joints.resize(rescue::mascot::kMaxJoints);
		context->UpdateSubresource(m_jointBuffer.get(), 0, nullptr, joints.data(), 0, 0);

		SceneConstants constants{};
		XMStoreFloat4x4(&constants.worldViewProjection, XMMatrixTranspose(world * viewProjection));
		XMStoreFloat4x4(&constants.world, XMMatrixTranspose(world));
		constants.cameraPosition = cameraPosition;
		context->UpdateSubresource(m_constantBuffer.get(), 0, nullptr, &constants, 0, 0);

		constexpr UINT stride = sizeof(MeshVertex);
		constexpr UINT offset = 0;
		ID3D11Buffer* const vertexBuffer = m_vertexBuffer.get();
		// b0 と b1 と b2。頂点シェーダーも b2（輪郭線の太さ）を読む
		const std::array<ID3D11Buffer*, 3> constantBuffers{ m_constantBuffer.get(), m_jointBuffer.get(), m_materialBuffer.get() };
		ID3D11SamplerState* const sampler = m_sampler.get();
		context->IASetInputLayout(m_inputLayout.get());
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
		context->IASetIndexBuffer(m_indexBuffer.get(), DXGI_FORMAT_R32_UINT, 0);
		context->VSSetShader(m_vertexShader.get(), nullptr, 0);
		context->VSSetConstantBuffers(0, static_cast<UINT>(constantBuffers.size()), constantBuffers.data());
		context->VSSetSamplers(0, 1, &sampler);
		context->PSSetShader(m_pixelShader.get(), nullptr, 0);
		context->PSSetConstantBuffers(0, static_cast<UINT>(constantBuffers.size()), constantBuffers.data());	// ピクセルシェーダーも b0（カメラの位置）と b2 を読む
		context->PSSetSamplers(0, 1, &sampler);

		// 描く順番:
		// 1. 不透明と切り抜きの面 → 2. その輪郭線 → 3. 半透明（renderQueueOffset の順）の面と輪郭線。
		// 半透明は「後ろの色」と混ぜるので、後ろにあるもの（不透明のもの）を先に描き終えておく必要がある
		for (const Part& part : m_parts) {
			if (part.alphaMode != AlphaMode::Blend) DrawPart(context, part, false);
		}
		for (const Part& part : m_parts) {
			if (part.alphaMode != AlphaMode::Blend && part.hasOutline) DrawPart(context, part, true);
		}
		for (const std::size_t i : m_blendOrder) {
			DrawPart(context, m_parts[i], false);
			if (m_parts[i].hasOutline) DrawPart(context, m_parts[i], true);
		}

		// 道具を既定に戻す（次に使う人が困らないように）
		context->RSSetState(nullptr);
		context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		context->OMSetDepthStencilState(nullptr, 0);
	}

	// 読んで手持ちに足す。再生は PlayMotion
	// ファイルを読むのは呼ぶ側（LoadMotionAsset か LoadGltfMotion）。name はログ用
	std::wstring Model::AddMotion(MotionData motion, std::wstring_view name) {
		THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), m_humanBones.empty(), "the model has no humanoid (not a VRM?)");
		const std::size_t motionBones = motion.humanBones.size();
		const std::size_t motionExpressions = motion.expressions.size();
		// モデルの表情の名前（モーションの表情と突き合わせる）
		std::vector<std::wstring> expressionNames;
		for (const MeshExpression& expression : m_expressions) expressionNames.push_back(expression.name);
		const MotionPlayer& player = m_motions.emplace_back(std::move(motion), m_skeleton, m_humanBones, expressionNames);
		return std::format(L"{} / {:.2f}秒 / 人型の骨{}本のうち{}本をモデルに移す / 表情{}個",
			name, player.Duration(), motionBones, player.BoneCount(), motionExpressions);
	}

	// index 番を再生する。
	// 現在のモーションを m_fadingOut に移し、クロスフェード中だけ直前まで表示していた動きの続きを再生する。
	// そのため、始めた時刻を「繰り返しの何周目か」を除いた時刻に直す（前のモーションの時刻が、切り替えた瞬間から続くように）。
	// 1 本が終わったちょうどで切り替えたときは、「次の周の 0 秒目」ではなく「この周の最後」とみなす（ceil - 1。
	// fmod で余りを取ると、終わりで切り替えたときに前のモーションの最初の姿勢から混ぜてしまう）
	void Model::PlayMotion(std::size_t index, float fadeSeconds, std::optional<float> start) {
		THROW_HR_IF(E_BOUNDS, index >= m_motions.size());
		m_fadingOut.reset();
		if (m_motion && m_motion->start && m_lastSeconds) {
			const float duration = (std::max)(m_motions[m_motion->index].Duration(), 1e-3f);
			const float elapsed = (std::max)(*m_lastSeconds - *m_motion->start, 0.0f);
			const float loops = (std::max)(std::ceil(elapsed / duration) - 1.0f, 0.0f);	// 済んだ周の数
			m_motion->start = *m_motion->start + loops * duration;
			m_fadingOut = m_motion;
		}
		m_motion = PlayingMotion{ index, start };
		// 前に見せていた姿勢が無いときは、基本の姿勢（T ポーズ）から混ぜることになるので、つながない
		m_fadeSeconds = m_fadingOut ? fadeSeconds : 0.0f;
		m_fadeStart.reset();	// 次の Draw の時刻から数える
	}

	// 名前の表情の効き具合を覚える。同じ値なら何もしない（頂点の書き直しは重いので、変わったときだけ）
	bool Model::SetExpression(std::wstring_view name, float weight) {
		const auto found = std::ranges::find(m_expressions, name, &MeshExpression::name);
		if (found == m_expressions.end()) return false;
		float& current = m_expressionWeights[static_cast<std::size_t>(found - m_expressions.begin())];
		weight = std::clamp(weight, 0.0f, 1.0f);
		if (current != weight) {
			current = weight;
			m_morphDirty = true;
		}
		return true;
	}

	// (1)表情の効き具合 → モーフごとの効き具合 (2)元の頂点をモーフでずらす (3)頂点バッファに書き直す
	void Model::ApplyMorphs(ID3D11DeviceContext* context) {
		m_morphDirty = false;

		// 1. 表情は複数のモーフを動かし、同じモーフを複数の表情が動かすこともある。モーフごとに足し合わせる。
		// isBinary の表情は 0 か 1 にする
		// モーションが動かしている表情は、自分で決めた効き具合（まばたきなど）より、モーションのほうを使う。
		// クロスフェード中は control が 0〜1 の間にあるため、両方を混ぜる（モーションが目を閉じている間は、まばたきを止める）
		std::vector<float> targetWeights(m_morphTargets.size(), 0.0f);
		for (std::size_t e = 0; e < m_expressions.size(); ++e) {
			const float control = std::clamp(m_motionExpressionControl[e], 0.0f, 1.0f);
			const float mixed = std::clamp(m_expressionWeights[e] * (1.0f - control) + m_motionExpressionWeights[e], 0.0f, 1.0f);
			const float weight = m_expressions[e].isBinary ? (mixed > 0.5f ? 1.0f : 0.0f) : mixed;
			if (weight <= 0.0f) continue;
			for (const MeshExpression::Morph& morph : m_expressions[e].morphs) targetWeights[morph.target] += weight * morph.weight;
		}

		// 2. 元の頂点から毎回計算し直す（前の結果に足すと、表情を戻したときに元に戻らない）
		m_morphedVertices = m_baseVertices;
		for (std::size_t t = 0; t < m_morphTargets.size(); ++t) {
			const float weight = targetWeights[t];
			if (weight == 0.0f) continue;
			for (const MorphDelta& delta : m_morphTargets[t].deltas) {
				MeshVertex& v = m_morphedVertices[delta.vertex];
				// 頂点の位置を「ずらす量 × 効き具合」だけ動かす
				XMStoreFloat3(&v.position, XMLoadFloat3(&v.position) + XMLoadFloat3(&delta.position) * weight);
				XMStoreFloat3(&v.normal, XMLoadFloat3(&v.normal) + XMLoadFloat3(&delta.normal) * weight);	// 法線も同じように（陰影がずれないように）
			}
		}

		// 3. 頂点バッファに書き直す。Map で GPU のバッファを CPU から書ける場所として借り、書いたら Unmap で返す。
		// WRITE_DISCARD は「前の中身は捨ててよい」。GPU が前のフレームを描いている途中でも、待たずに新しい場所を貸してくれる
		D3D11_MAPPED_SUBRESOURCE mapped{};
		THROW_IF_FAILED(context->Map(m_vertexBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		std::memcpy(mapped.pData, m_morphedVertices.data(), m_morphedVertices.size() * sizeof(MeshVertex));
		context->Unmap(m_vertexBuffer.get(), 0);
	}

	// 部分 1 つを描く: 材質を b2 に送り、裏も描くか・混ぜるかの道具を付けて、テクスチャを付け替えて、その範囲の三角形だけ描く。
	// outline なら輪郭線（太らせて裏だけを描く）
	void Model::DrawPart(ID3D11DeviceContext* context, const Part& part, bool outline) {
		MaterialConstants material = part.constants;
		material.isOutline = outline ? 1 : 0;
		context->UpdateSubresource(m_materialBuffer.get(), 0, nullptr, &material, 0, 0);

		const bool blend = part.alphaMode == AlphaMode::Blend;
		context->RSSetState(m_rasterizers[outline ? 2 : part.doubleSided ? 1 : 0].get());
		context->OMSetBlendState(blend ? m_blend.get() : nullptr, nullptr, 0xffffffff);
		context->OMSetDepthStencilState(blend ? m_depthReadOnly.get() : nullptr, 0);

		// t0〜t5（ピクセルシェーダー）。影の境目の画像だけは数として読む
		const auto color = [&](TextureSlot slot) { return m_textures[part.textures[slot]].color.get(); };
		const auto value = [&](TextureSlot slot) { return m_textures[part.textures[slot]].value.get(); };
		const std::array<ID3D11ShaderResourceView*, 6> pixelTextures{
			color(BaseColor), color(ShadeMultiply), value(ShadingShift), color(Matcap), color(RimMultiply), color(Emissive) };
		context->PSSetShaderResources(0, static_cast<UINT>(pixelTextures.size()), pixelTextures.data());
		// t6（頂点シェーダー）: 輪郭線の太さの画像。これも数
		ID3D11ShaderResourceView* const outlineTexture = value(OutlineWidth);
		context->VSSetShaderResources(6, 1, &outlineTexture);

		context->DrawIndexed(part.indexCount, part.firstIndex, 0);
	}
}
