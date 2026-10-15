#include "mascot/GltfMesh.h"

#include <windows.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstddef>
#include <cstring>
#include <format>
#include <optional>
#include <map>
#include <span>
#include <string_view>
#include <utility>

#include <winrt/base.h>		// winrt::hresult_error（ParseMToon の例外）を HRESULT にするため
#include <winrt/windows.foundation.collections.h>	// ParseMotionExpressions（Humanoid.cpp と同じ読み方）
#include <winrt/windows.data.json.h>

#include "mascot/Expression.h"
#include "mascot/Humanoid.h"
#include "mascot/ImageDecode.h"
#include "mascot/SpringBone.h"
#include "cgltf/cgltf.h"		// third_party/cgltf。関数の中身は CgltfImpl.cpp で 1 回だけ作る（ヘッダー 1 つのライブラリ）

namespace {
	using namespace DirectX;
	using rescue::mascot::MeshVertex;

	// cgltf_data* を cgltf_free で解放する箱（wil::unique_any は「型と解放の関数」を指定して箱を作れる）
	using unique_cgltf_data = wil::unique_any<cgltf_data*, decltype(&cgltf_free), cgltf_free>;

	// cgltf の結果を HRESULT にして例外にする。メッセージは英語（WIL のメッセージは日本語が化けるため）
	void ThrowIfCgltfFailed(cgltf_result result, const char* what) {
		if (result == cgltf_result_success) return;
		const HRESULT hr = result == cgltf_result_file_not_found ? HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) :
			result == cgltf_result_out_of_memory ? E_OUTOFMEMORY :
			HRESULT_FROM_WIN32(ERROR_INVALID_DATA);		//形式が壊れている・glTFではないなど
		THROW_HR_MSG(hr, "%hs failed (cgltf_result %d)", what, static_cast<int>(result));
	}

	// ファイルを丸ごと読む。cgltf にもファイルを開く関数はあるが、中で fopen を使うので日本語のパスを開けない。
	// そこで Win32（ワイド文字のパス）で読み、メモリから cgltf_parse に渡す。
	std::vector<std::byte> ReadAllBytes(const std::filesystem::path& path) {
		wil::unique_hfile file{ CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr) };
		THROW_LAST_ERROR_IF(!file);

		LARGE_INTEGER size{};
		THROW_IF_WIN32_BOOL_FALSE(GetFileSizeEx(file.get(), &size));
		THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE), size.QuadPart > 256 * 1024 * 1024);	// 256 MB まで

		std::vector<std::byte> bytes(static_cast<size_t>(size.QuadPart));
		DWORD read = 0;
		THROW_IF_WIN32_BOOL_FALSE(ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr));
		THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_HANDLE_EOF), read != bytes.size());
		return bytes;
	}

	// 属性（POSITION / NORMAL など。0 番目のもの）の中身が入っている accessor を探す。無ければ nullptr
	const cgltf_accessor* FindAttribute(const cgltf_primitive& primitive, cgltf_attribute_type type) {
		for (const cgltf_attribute& attribute : std::span{ primitive.attributes,primitive.attributes_count }) {
			if (attribute.type == type && attribute.index == 0) return attribute.data;
		}
		return nullptr;
	}

	// 材質の MToon を読む。cgltf は知らない拡張を「名前と JSON の文字列」のまま extensions に入れておいてくれるので、
	// VRMC_materials_mtoon を探して ParseMToon（MToon.cpp）に渡す。
	// テクスチャの番号は glTF の textures の番号なので、ここで images の番号に置き換える（Model は画像の番号しか知らない）
	std::optional<rescue::mascot::MToonMaterial> ReadMToon(const cgltf_data& data, const cgltf_material& material) {
		for (const cgltf_extension& extension : std::span{ material.extensions, material.extensions_count }) {
			if (!extension.name || !extension.data || std::string_view{ extension.name } != "VRMC_materials_mtoon") continue;

			rescue::mascot::MToonMaterial mtoon;
			try {
				mtoon = rescue::mascot::ParseMToon(extension.data);
			}
			catch (const winrt::hresult_error& e) {
				//C++/WinRT の例外は WIL の例外と別物なので、ここで WIL の例外に付け替える（どの材質かを添えて）
				THROW_HR_MSG(static_cast<HRESULT>(e.code()), "material '%hs': cannot read VRMC_materials_mtoon", material.name ? material.name : "");
			}

			const auto toImage = [&](std::optional<std::size_t>& texture) {
				if (!texture) return;
				texture = *texture < data.textures_count && data.textures[*texture].image
					? std::optional{ cgltf_image_index(&data, data.textures[*texture].image) }
					: std::nullopt;
			};
			toImage(mtoon.shadeMultiplyTexture);
			toImage(mtoon.shadingShiftTexture);
			toImage(mtoon.matcapTexture);
			toImage(mtoon.rimMultiplyTexture);
			toImage(mtoon.outlineWidthMultiplyTexture);
			return mtoon;
		}
		return std::nullopt;
	}

	XMFLOAT4X4 Identity() {
		XMFLOAT4X4 m;
		XMStoreFloat4x4(&m, XMMatrixIdentity());
		return m;
	}

	// ノードの木（親・基本の置き方）と、1 つ目のアニメーションを読む。パレットと normalize は LoadGltfMesh が足す
	rescue::mascot::Skeleton ReadSkeleton(const cgltf_data& data) {
		using namespace rescue::mascot;
		Skeleton skeleton;
		const std::span nodes{ data.nodes, data.nodes_count };
		skeleton.parents.resize(nodes.size());
		skeleton.rest.resize(nodes.size());
		std::vector<int> depth(nodes.size());	// 根からの深さ（親が子より先に来る順に並べるため）

		for (std::size_t i = 0; i < nodes.size(); ++i) {
			const cgltf_node& node = nodes[i];
			skeleton.parents[i] = node.parent ? static_cast<int>(cgltf_node_index(&data, node.parent)) : -1;
			for (const cgltf_node* p = node.parent; p; p = p->parent) ++depth[i];

			//置き方は「移動・回転・拡大」か「行列」のどちらかで書かれている。行列なら 3 つに分解して持つ
			NodeTransform& t = skeleton.rest[i];
			if (node.has_matrix) {
				const XMFLOAT4X4 matrix{ node.matrix };	// glTF の 16 個（列優先）をそのまま読めば DirectXMath の行列（行優先で、行ベクトルを掛ける）になる
				XMVECTOR s, r, tr;
				THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), !XMMatrixDecompose(&s, &r, &tr, XMLoadFloat4x4(&matrix)),
					"node %zu: matrix cannot be decomposed", i);
				XMStoreFloat3(&t.scale, s);
				XMStoreFloat4(&t.rotation, r);
				XMStoreFloat3(&t.translation, tr);
			}
			else {
				if (node.has_translation) t.translation = { node.translation[0], node.translation[1], node.translation[2] };
				if (node.has_rotation) t.rotation = { node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3] };
				if (node.has_scale) t.scale = { node.scale[0], node.scale[1], node.scale[2] };
			}
		}
		skeleton.order.resize(nodes.size());
		for (std::size_t i = 0; i < nodes.size(); ++i) skeleton.order[i] = i;
		std::ranges::stable_sort(skeleton.order, {}, [&](std::size_t i) { return depth[i]; });

		if (data.animations_count == 0) return skeleton;
		const cgltf_animation& animation = data.animations[0];
		for (const cgltf_animation_channel& source : std::span{ animation.channels, animation.channels_count }) {
			if (!source.target_node || !source.sampler) continue;
			AnimationChannel channel;
			switch (source.target_path) {
			case cgltf_animation_path_type_translation: channel.path = AnimationPath::Translation; break;
			case cgltf_animation_path_type_rotation:    channel.path = AnimationPath::Rotation; break;
			case cgltf_animation_path_type_scale:       channel.path = AnimationPath::Scale; break;
			default: continue;	// weights（表情などのモーフ）はまだ扱わない
			}
			switch (source.sampler->interpolation) {
			case cgltf_interpolation_type_step:         channel.interpolation = Interpolation::Step; break;
			case cgltf_interpolation_type_cubic_spline: channel.interpolation = Interpolation::CubicSpline; break;
			default:                                    channel.interpolation = Interpolation::Linear; break;
			}
			channel.node = cgltf_node_index(&data, source.target_node);

			//時刻（input）と値（output）。個数が合っているかは cgltf_validate が確かめてある
			const cgltf_accessor* input = source.sampler->input;
			const cgltf_accessor* output = source.sampler->output;
			if (input->count == 0) continue;
			channel.times.resize(input->count);
			for (cgltf_size k = 0; k < input->count; ++k) cgltf_accessor_read_float(input, k, &channel.times[k], 1);
			const cgltf_size components = channel.path == AnimationPath::Rotation ? 4 : 3;
			channel.values.resize(output->count);
			for (cgltf_size k = 0; k < output->count; ++k) cgltf_accessor_read_float(output, k, &channel.values[k].x, components);

			skeleton.duration = (std::max)(skeleton.duration, channel.times.back());
			skeleton.channels.push_back(std::move(channel));
		}
		return skeleton;
	}

	// VRMC_vrm_animation の expressions を読む（読み方は Humanoid.cpp の ParseHumanBones と同じ）。
	// 形は { "expressions": { "preset": { "blink": { "node": 50 }, ... }, "custom": { "名前": { "node": 51 }, ... } } }。
	// preset と custom を分けずに、名前とノードの組にする（モデルの表情も同じ名前で持っている）。無ければ空
	std::vector<rescue::mascot::MotionExpression> ParseMotionExpressions(std::string_view json) {
		using winrt::Windows::Data::Json::JsonObject;
		const JsonObject root = JsonObject::Parse(winrt::to_hstring(json));
		std::vector<rescue::mascot::MotionExpression> expressions;
		if (!root.HasKey(L"expressions")) return expressions;
		const JsonObject all = root.GetNamedObject(L"expressions");
		for (const wchar_t* group : { L"preset", L"custom" }) {
			if (!all.HasKey(group)) continue;
			for (const auto& [name, value] : all.GetNamedObject(group)) {
				const double node = value.GetObject().GetNamedNumber(L"node");
				if (node < 0) continue;
				expressions.push_back({ std::wstring{ name }, static_cast<std::size_t>(node) });
			}
		}
		return expressions;
	}

	// メモリ上の .glb / .vrm / .vrma を読む。cgltf_parse は JSON を読むだけで、頂点などの中身（バッファ）は cgltf_load_buffers で付ける。
	// .glb は中身もファイルの中にある。外のファイルを指すバッファは読まない（他人のファイルに、別のファイルを読ませないため）。
	// 結果は bytes を指したままなので、bytes は結果より長く生かす
	unique_cgltf_data ParseGlb(const std::vector<std::byte>& bytes) {
		cgltf_options options{};
		unique_cgltf_data data;
		ThrowIfCgltfFailed(cgltf_parse(&options, bytes.data(), bytes.size(), data.put()), "cgltf_parse");
		for (const cgltf_buffer& buffer : std::span{ data.get()->buffers, data.get()->buffers_count }) {
			THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), buffer.uri && std::strncmp(buffer.uri, "data:", 5) != 0,
				"external buffer '%hs' is not supported (use .glb)", buffer.uri);
		}
		ThrowIfCgltfFailed(cgltf_load_buffers(&options, data.get(), nullptr), "cgltf_load_buffers");
		ThrowIfCgltfFailed(cgltf_validate(data.get()), "cgltf_validate");
		return data;
	}
}


namespace rescue::mascot {

	MeshData LoadGltfMesh(const std::filesystem::path& path) {
		// 1. 読み込む
		const std::vector<std::byte> bytes = ReadAllBytes(path);
		const unique_cgltf_data data = ParseGlb(bytes);

		// 2. 骨とアニメーションを先に読む（頂点の joints が、骨の行列の配列＝パレットの番号を指すため）
		MeshData mesh;
		mesh.skeleton = ReadSkeleton(*data.get());
		std::vector<PaletteEntry>& palette = mesh.skeleton.palette;
		std::vector<std::optional<std::uint32_t>> skinOffsets(data.get()->skins_count);	// スキンごとの、パレットでの始まり

		// 3. メッシュを持つノードを全部たどり、三角形の頂点を 1 本の配列にまとめる。
		// 骨で動かすので、頂点は「メッシュの座標」のまま持ち、置き方（骨の動き）は毎フレーム GPU で掛ける。どの行列を掛けるかは頂点ごとの joints（パレットの番号 4 つ）と weights（混ぜる割合）
		size_t meshCount = 0;
		// （ノードの番号, モーフの番号）→ morphTargets の番号（プリミティブの数だけある）。(6)で VRM の表情を置き換えるのに使う
		std::map<std::pair<std::size_t, std::size_t>, std::vector<std::size_t>> morphsOfNode;
		for (const cgltf_node& node : std::span{ data.get()->nodes, data.get()->nodes_count }) {
			if (!node.mesh) continue;
			++meshCount;
			const std::size_t nodeIndex = cgltf_node_index(data.get(), &node);

			//骨の付いていないメッシュは、そのノード自身を「骨 1 本」としてパレットに足し、全頂点をそれに 100% 付ける
			const auto staticEntry = static_cast<std::uint32_t>(palette.size());
			palette.push_back({ cgltf_node_index(data.get(), &node), Identity() });

			//骨の付いたメッシュは、スキン（骨の一覧と逆バインド行列）をパレットに足す。同じスキンを使うメッシュが他にもあれば使い回す
			std::optional<std::uint32_t> skinOffset;
			if (node.skin) {
				std::optional<std::uint32_t>& offset = skinOffsets[cgltf_skin_index(data.get(), node.skin)];
				if (!offset) {
					offset = static_cast<std::uint32_t>(palette.size());
					for (cgltf_size j = 0; j < node.skin->joints_count; ++j) {
						XMFLOAT4X4 inverseBind = Identity();
						if (node.skin->inverse_bind_matrices) cgltf_accessor_read_float(node.skin->inverse_bind_matrices, j, &inverseBind.m[0][0], 16);
						palette.push_back({ cgltf_node_index(data.get(), node.skin->joints[j]), inverseBind });
					}
				}
				skinOffset = offset;
			}

			for (const cgltf_primitive& primitive : std::span{ node.mesh->primitives, node.mesh->primitives_count }) {
				if (primitive.type != cgltf_primitive_type_triangles) continue;	// 線や点は描かない
				const cgltf_accessor* positions = FindAttribute(primitive, cgltf_attribute_type_position);
				const cgltf_accessor* normals = FindAttribute(primitive, cgltf_attribute_type_normal);
				const cgltf_accessor* joints = FindAttribute(primitive, cgltf_attribute_type_joints);
				const cgltf_accessor* weights = FindAttribute(primitive, cgltf_attribute_type_weights);
				if (!positions) continue;
				const bool skinned = skinOffset && joints && weights;

				//色は材質の基本色。テクスチャがあれば、シェーダーでテクスチャの色に掛ける（基本色は「全体の色味」の役になる）
				XMFLOAT3 color{ 1, 1, 1 };
				MeshPart part;							// 範囲は最後に決める。ここでは材質の透明の扱いを写す
				std::optional<std::size_t>& image = part.image;		// 貼る画像の番号（MeshData::images の添字）
				const cgltf_accessor* texcoords = nullptr;
				if (primitive.material && primitive.material->has_pbr_metallic_roughness) {
					const cgltf_pbr_metallic_roughness& pbr = primitive.material->pbr_metallic_roughness;
					color = { pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2] };
					part.alpha = pbr.base_color_factor[3];

					//材質 → テクスチャ → 画像、とたどる。UV は TEXCOORD_n のどれを使うかが材質に書いてある（たいてい 0）
					const cgltf_texture_view& view = pbr.base_color_texture;
					if (view.texture && view.texture->image) {
						image = cgltf_image_index(data.get(), view.texture->image);
						for (const cgltf_attribute& attribute : std::span{ primitive.attributes, primitive.attributes_count }) {
							if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == view.texcoord) texcoords = attribute.data;
						}
						if (!texcoords) image.reset();	// UV が無ければ貼れないので、色だけにする
					}
				}

				//透明の扱い。cgltf の列挙を自分の列挙に置き換える（cgltf の型を GltfMesh.cpp の外に出さない）
				if (primitive.material) {
					const cgltf_material& material = *primitive.material;
					part.alphaMode = material.alpha_mode == cgltf_alpha_mode_mask ? AlphaMode::Mask
						: material.alpha_mode == cgltf_alpha_mode_blend ? AlphaMode::Blend
						: AlphaMode::Opaque;
					part.alphaCutoff = material.alpha_cutoff;
					part.doubleSided = material.double_sided != 0;

					//自分で光る色と、MToon。テクスチャの UV は基本色と同じ TEXCOORD_0 を使う前提（VRM はそう）
					part.emissive = { material.emissive_factor[0], material.emissive_factor[1], material.emissive_factor[2] };
					if (material.emissive_texture.texture && material.emissive_texture.texture->image) {
						part.emissiveImage = cgltf_image_index(data.get(), material.emissive_texture.texture->image);
					}
					part.mtoon = ReadMToon(*data.get(), material);
				}

				//このプリミティブの頂点が、配列の何番目から始まるか（三角形の番号をずらすのに使う）
				const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
				for (cgltf_size i = 0; i < positions->count; ++i) {
					MeshVertex v{};
					v.normal = { 0, 1, 0 };		// 法線が無いファイルは上向きにしておく
					v.color = color;
					v.joints = { staticEntry, 0, 0, 0 };
					v.weights = { 1, 0, 0, 0 };
					cgltf_accessor_read_float(positions, i, &v.position.x, 3);
					if (normals) cgltf_accessor_read_float(normals, i, &v.normal.x, 3);
					if (texcoords) {
						cgltf_accessor_read_float(texcoords, i, &v.uv.x, 2);
						// UV の上下はそのまま。glTF の UV も D3D のテクスチャも左上が 0,0
					}
					//骨の番号はスキンの中での番号なので、パレットでの始まりを足す
					if (skinned) {
						cgltf_uint j[4]{};
						cgltf_accessor_read_uint(joints, i, j, 4);
						v.joints = { *skinOffset + j[0], *skinOffset + j[1], *skinOffset + j[2], *skinOffset + j[3] };
						cgltf_accessor_read_float(weights, i, &v.weights.x, 4);
					}
					mesh.vertices.push_back(v);
				}

				//このプリミティブのモーフ。モーフは「頂点ごとのずらす量」を、元の頂点と同じ並びで持っている。
				// ほとんどの頂点はずれない（目を閉じるモーフなら、まぶたの周りだけ）ので、ずれる頂点だけを覚える
				for (cgltf_size t = 0; t < primitive.targets_count; ++t) {
					const cgltf_morph_target& source = primitive.targets[t];
					const cgltf_accessor* deltaPositions = nullptr;
					const cgltf_accessor* deltaNormals = nullptr;
					for (const cgltf_attribute& attribute : std::span{ source.attributes, source.attributes_count }) {
						if (attribute.type == cgltf_attribute_type_position) deltaPositions = attribute.data;
						if (attribute.type == cgltf_attribute_type_normal) deltaNormals = attribute.data;
					}
					MorphTarget target;
					for (cgltf_size i = 0; i < positions->count; ++i) {
						MorphDelta delta{ base + static_cast<std::uint32_t>(i) };
						if (deltaPositions && i < deltaPositions->count) cgltf_accessor_read_float(deltaPositions, i, &delta.position.x, 3);
						if (deltaNormals && i < deltaNormals->count) cgltf_accessor_read_float(deltaNormals, i, &delta.normal.x, 3);
						const bool moves = delta.position.x != 0 || delta.position.y != 0 || delta.position.z != 0
							|| delta.normal.x != 0 || delta.normal.y != 0 || delta.normal.z != 0;
						if (moves) target.deltas.push_back(delta);
					}
					morphsOfNode[{ nodeIndex, t }].push_back(mesh.morphTargets.size());
					mesh.morphTargets.push_back(std::move(target));
				}

				//三角形。番号（indices）が無いファイルは、頂点を 3 つずつ順に使う。
				// at は「k 番目の番号を、配列全体での頂点の番号にして返す」小さな関数（ラムダ）。同じ計算を 3 回するので 1 回だけ書く
				const cgltf_size count = primitive.indices ? primitive.indices->count : positions->count;
				const auto firstIndex = static_cast<std::uint32_t>(mesh.indices.size());	// この部分の始まり
				for (cgltf_size k = 0; k + 2 < count; k += 3) {
					const auto at = [&](cgltf_size j) {
						return base + static_cast<std::uint32_t>(primitive.indices ? cgltf_accessor_read_index(primitive.indices, j) : j);
					};
					// 三角形の頂点の順番を入れ替える。glTF は右手系、D3D は左手系なので Z を反転する（(4)の normalize 行列）。
					// 鏡に映すと表と裏が入れ替わるので、2 番目と 3 番目を入れ替えて表の向きを戻す
					mesh.indices.insert(mesh.indices.end(), { at(k), at(k + 2), at(k + 1) });
				}
				//ここまでに足した三角形を 1 つの部分として覚える（描くときは部分ごとにテクスチャを付け替える）
				const auto indexCount = static_cast<std::uint32_t>(mesh.indices.size()) - firstIndex;
				if (indexCount > 0) {
					part.firstIndex = firstIndex;
					part.indexCount = indexCount;
					mesh.parts.push_back(part);
				}
			}
		}
		THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), mesh.indices.empty(), "no triangles in the model");

		// VRM の表情。ファイル全体の拡張 VRMC_vrm を ParseExpressions（Expression.cpp）に渡し、
		// 「ノードとモーフの番号」を morphTargets の番号に置き換える。モデルに無いモーフを指していたら、その 1 つだけ飛ばす
		for (const cgltf_extension& extension : std::span{ data.get()->data_extensions, data.get()->data_extensions_count }) {
			if (!extension.name || !extension.data || std::string_view{ extension.name } != "VRMC_vrm") continue;
			std::vector<VrmExpression> expressions;
			try {
				expressions = ParseExpressions(extension.data);
				mesh.humanBones = ParseHumanBones(extension.data);	// 人型の骨の名前（Humanoid.cpp）
			}
			catch (const winrt::hresult_error& e) {
				THROW_HR_MSG(static_cast<HRESULT>(e.code()), "cannot read VRMC_vrm expressions");
			}
			for (const VrmExpression& source : expressions) {
				MeshExpression expression{ source.name, source.isBinary, {} };
				for (const MorphTargetBind& bind : source.binds) {
					const auto found = morphsOfNode.find({ bind.node, bind.index });
					if (found == morphsOfNode.end()) continue;
					for (const std::size_t target : found->second) expression.morphs.push_back({ target, bind.weight });
				}
				mesh.expressions.push_back(std::move(expression));
			}
		}
		// ファイル全体の拡張 VRMC_springBone を ParseSpringBones（SpringBone.cpp）に渡す。番号の範囲の確認は SpringSimulator がする
		for (const cgltf_extension& extension : std::span{ data.get()->data_extensions, data.get()->data_extensions_count }) {
			if (!extension.name || !extension.data || std::string_view{ extension.name } != "VRMC_springBone") continue;
			try {
				mesh.springBones = ParseSpringBones(extension.data);
			}
			catch (const winrt::hresult_error& e) {
				THROW_HR_MSG(static_cast<HRESULT>(e.code()), "cannot read VRMC_springBone");
			}
		}
		THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), palette.size() > kMaxJoints,
			"too many joints (%zu, up to %zu)", palette.size(), kMaxJoints);

		// 4. 大きさと位置をそろえる行列（normalize）を作る。頂点は「メッシュの座標」のままなので、
		// 基本の姿勢で骨の行列を掛けた位置（＝画面に出る形）を CPU で 1 回計算し、それを囲む箱から決める。
		// まず Z の反転（右手系 → 左手系）だけの normalize で基本の姿勢を計算する
		const XMMATRIX flipZ = XMMatrixScaling(1.0f, 1.0f, -1.0f);
		XMStoreFloat4x4(&mesh.skeleton.normalize, flipZ);
		const std::vector<XMFLOAT4X4> rest = mesh.skeleton.JointMatrices(std::nullopt);

		XMVECTOR lower = XMVectorReplicate(FLT_MAX);
		XMVECTOR upper = XMVectorReplicate(-FLT_MAX);
		for (const MeshVertex& v : mesh.vertices) {
			//シェーダーと同じ計算: 4 本の骨の行列で動かした位置を、weights の割合で混ぜる
			const XMVECTOR p = XMLoadFloat3(&v.position);
			const std::array<std::uint32_t, 4> joints{ v.joints.x, v.joints.y, v.joints.z, v.joints.w };
			const std::array<float, 4> weights{ v.weights.x, v.weights.y, v.weights.z, v.weights.w };
			XMVECTOR skinned = XMVectorZero();
			for (std::size_t k = 0; k < 4; ++k) {
				if (weights[k] > 0.0f) skinned += XMVector3TransformCoord(p, XMLoadFloat4x4(&rest[joints[k]])) * weights[k];
			}
			lower = XMVectorMin(lower, skinned);
			upper = XMVectorMax(upper, skinned);
		}
		//全体を囲む箱の中心を原点に、いちばん長い辺を 2（-1〜1）にする。そうすればカメラの位置を固定できる
		const XMVECTOR center = (lower + upper) * 0.5f;
		//その場で呼ぶラムダ: 数行かかる計算の結果を const の変数に入れたいときの書き方（最後の () ですぐ呼んでいる）
		const XMFLOAT3 size = [&] { XMFLOAT3 s; XMStoreFloat3(&s, upper - lower); return s; }();
		//windows.h が max という名前のマクロを作るので、std::max(...) と書くとマクロに化ける。(std::max) とカッコで囲むとマクロにならない
		const float scale = 2.0f / (std::max)({ size.x, size.y, size.z });
		//行ベクトルなので、左から順に効く: Z の反転 → 中心を原点へ → 拡大
		XMStoreFloat4x4(&mesh.skeleton.normalize, flipZ * XMMatrixTranslationFromVector(-center) * XMMatrixScaling(scale, scale, scale));
		// 箱の半分の大きさ（そろえた後）の長さ = 中心から角まで。マスコットの窓の大きさを決めるのに使う
		mesh.radius = XMVectorGetX(XMVector3Length((upper - lower) * 0.5f * scale));

		// 5. 画像ファイルの中身を画素に戻す。.glb では画像も中身（バッファ）の一部で、buffer_view が「どこから何バイト」を指す。
		// 番号（添字）を data->images と同じにしたいので、読めない画像（URI で外を指すものなど）も空のまま 1 つ数える。
		// 変換した形式には画素で入れるので、ここで戻す
		size_t imageCount = 0;
		for (const cgltf_image& source : std::span{ data.get()->images, data.get()->images_count }) {
			DecodedImage& image = mesh.images.emplace_back();
			if (!source.buffer_view) continue;
			const auto* begin = reinterpret_cast<const std::byte*>(cgltf_buffer_view_data(source.buffer_view));
			image = DecodeImage(std::span{ begin, source.buffer_view->size });
			++imageCount;
		}

		mesh.info = std::format(L"メッシュ{}個 / 頂点{}個 / 三角形{}個 / 部分{}個 / 画像{}枚 / 大きさ {:.2f} x {:.2f} x {:.2f} / 骨の行列{}個 / アニメーション{}本 {:.2f}秒 / モーフ{}個 / 表情{}個 / 揺れもの{}本 / 当たり判定{}個",
			meshCount, mesh.vertices.size(), mesh.indices.size() / 3, mesh.parts.size(), imageCount, size.x, size.y, size.z,
			palette.size(), mesh.skeleton.channels.size(), mesh.skeleton.duration, mesh.morphTargets.size(), mesh.expressions.size(),
			mesh.springBones.springs.size(), mesh.springBones.colliders.size());
		//data（cgltf の読み込み結果）は return で抜けるときに解放される。必要なものは mesh に写した
		return mesh;
	}

	// .vrma を読む。読み込みは LoadGltfMesh と同じ（メッシュは無いので、骨とアニメーションと人型の骨と表情だけ）
	MotionData LoadGltfMotion(const std::filesystem::path& path) {
		const std::vector<std::byte> bytes = ReadAllBytes(path);
		const unique_cgltf_data data = ParseGlb(bytes);

		MotionData motion;
		motion.skeleton = ReadSkeleton(*data.get());
		for (const cgltf_extension& extension : std::span{ data.get()->data_extensions, data.get()->data_extensions_count }) {
			if (!extension.name || !extension.data || std::string_view{ extension.name } != "VRMC_vrm_animation") continue;
			try {
				motion.humanBones = ParseHumanBones(extension.data);
				motion.expressions = ParseMotionExpressions(extension.data);
			}
			catch (const winrt::hresult_error& e) {
				THROW_HR_MSG(static_cast<HRESULT>(e.code()), "cannot read VRMC_vrm_animation humanoid / expressions");
			}
		}
		THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), motion.humanBones.empty(), "no VRMC_vrm_animation humanoid (not a .vrma?)");
		THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), motion.skeleton.channels.empty(), "no animation");
		return motion;
	}
}
