// 自前の形式の読み書き（流れは AssetFile.h）。
//   Writer / Reader: 数・中身・文字列・番号（optional）を並べる道具。Reader は数を読むたびに残りのバイト数と比べる
//   Write* / Read*: 構造体ごとに、決まった順で並べる・読む（書く順と読む順は必ず同じにする）
//   Validate*: 読み終えた後に、添字に使う番号が配列の中を指しているかを全部確かめる
// メモリのまま書き読みしてよい型は、requires と std::is_trivially_copyable_v（memcpy してよい型か）で絞る
#include "mascot/AssetFile.h"

#include <windows.h>
#include <wil/result.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <type_traits>
#include <variant>

#include "config/MascotSettings.h"

namespace {
	using namespace rescue::mascot;
	namespace config = rescue::config;

	const HRESULT kInvalidData = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
	constexpr std::uint32_t kMaxImageSide = 16384;	// D3D11 のテクスチャの上限

	// ---- 書く道具 ----
	class Writer {
	public:
		explicit Writer(const std::filesystem::path& path) : m_out{ path, std::ios::binary | std::ios::trunc } {
			THROW_HR_IF_MSG(E_FAIL, !m_out, "cannot open the output file");
		}

		// memcpy してよい型（数・XMFLOAT3・MeshVertex など）は、メモリのまま書く
		template <typename T> requires std::is_trivially_copyable_v<T>
		void Value(const T& value) { Raw(&value, sizeof(T)); }

		void Count(std::size_t count) { Value<std::uint64_t>(count); }
		void Bool(bool value) { Value<std::uint8_t>(value ? 1 : 0); }

		// 数と、中身をメモリのまま
		template <typename T> requires std::is_trivially_copyable_v<T>
		void Array(const std::vector<T>& values) {
			Count(values.size());
			Raw(values.data(), values.size() * sizeof(T));
		}

		void String(const std::wstring& text) {
			Count(text.size());
			Raw(text.data(), text.size() * sizeof(wchar_t));
		}

		// 無いものは -1
		void Index(const std::optional<std::size_t>& index) { Value<std::int64_t>(index ? static_cast<std::int64_t>(*index) : -1); }

		void Finish() {
			m_out.flush();
			THROW_HR_IF_MSG(E_FAIL, !m_out, "failed to write the output file");
		}

	private:
		void Raw(const void* data, std::size_t bytes) {
			if (bytes > 0) m_out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
		}

		std::ofstream m_out;
	};

	// ---- 読む道具 ----
	// 数を読むたびに「その数だけの中身が、残りのバイト数に入りきるか」を確かめる。
	// 入りきらない数（壊れたファイル・わざと大きくした数）で、巨大なメモリを取ろうとしないため
	class Reader {
	public:
		explicit Reader(const std::filesystem::path& path) : m_in{ path, std::ios::binary } {
			THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !m_in, "cannot open the asset file");
			m_left = std::filesystem::file_size(path);
		}

		template <typename T> requires std::is_trivially_copyable_v<T>
		T Value() {
			T value;
			Raw(&value, sizeof(T));
			return value;
		}

		// 数を読み、1 つあたり少なくとも minBytes 使うものが、残りに入りきるか確かめる
		std::size_t Count(std::size_t minBytes) {
			const std::uint64_t count = Value<std::uint64_t>();
			THROW_HR_IF_MSG(kInvalidData, minBytes > 0 && count > m_left / minBytes, "a count is larger than the file");
			return static_cast<std::size_t>(count);
		}

		bool Bool() {
			const auto value = Value<std::uint8_t>();
			THROW_HR_IF_MSG(kInvalidData, value > 1, "a bool is neither 0 nor 1");
			return value == 1;
		}

		template <typename T> requires std::is_trivially_copyable_v<T>
		std::vector<T> Array() {
			std::vector<T> values(Count(sizeof(T)));
			Raw(values.data(), values.size() * sizeof(T));
			return values;
		}

		std::wstring String() {
			std::wstring text(Count(sizeof(wchar_t)), L'\0');
			Raw(text.data(), text.size() * sizeof(wchar_t));
			return text;
		}

		std::optional<std::size_t> Index() {
			const auto value = Value<std::int64_t>();
			if (value == -1) return std::nullopt;
			THROW_HR_IF_MSG(kInvalidData, value < 0, "a negative index");
			return static_cast<std::size_t>(value);
		}

		// enum class を、最大値 last までなら受け取る
		template <typename E>
		E Enum(E last) {
			const auto value = Value<std::uint32_t>();
			THROW_HR_IF_MSG(kInvalidData, value > static_cast<std::uint32_t>(last), "an enum value out of range");
			return static_cast<E>(value);
		}

		void ExpectEnd() const { THROW_HR_IF_MSG(kInvalidData, m_left != 0, "extra bytes at the end"); }

	private:
		void Raw(void* data, std::size_t bytes) {
			THROW_HR_IF_MSG(kInvalidData, bytes > m_left, "the file ends too early");
			if (bytes == 0) return;
			m_in.read(static_cast<char*>(data), static_cast<std::streamsize>(bytes));
			THROW_HR_IF_MSG(kInvalidData, !m_in, "failed to read the file");
			m_left -= bytes;
		}

		std::ifstream m_in;
		std::uint64_t m_left = 0;
	};

	// ---- 先頭の印と版 ----
	void WriteHeader(Writer& out, const char (&magic)[8]) {
		out.Value(magic);
		out.Value(config::kMascotAssetVersion);
	}

	void ReadHeader(Reader& in, const char (&magic)[8]) {
		const auto found = in.Value<std::array<char, 8>>();
		THROW_HR_IF_MSG(kInvalidData, std::memcmp(found.data(), magic, 8) != 0, "not a DisplayRescue asset file");
		THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), in.Value<std::uint32_t>() != config::kMascotAssetVersion,
			"the asset was converted by another version (convert it again)");
	}

	// ---- 骨とアニメーション（モデルとモーションで共通） ----
	void WriteSkeleton(Writer& out, const Skeleton& s) {
		out.Array(s.parents);
		out.Array(s.order);
		out.Array(s.rest);
		out.Count(s.channels.size());
		for (const AnimationChannel& c : s.channels) {
			out.Value<std::uint64_t>(c.node);
			out.Value(static_cast<std::uint32_t>(c.path));
			out.Value(static_cast<std::uint32_t>(c.interpolation));
			out.Array(c.times);
			out.Array(c.values);
		}
		out.Value(s.duration);
		out.Array(s.palette);
		out.Value(s.normalize);
	}

	Skeleton ReadSkeleton(Reader& in) {
		Skeleton s;
		s.parents = in.Array<int>();
		s.order = in.Array<std::size_t>();
		s.rest = in.Array<NodeTransform>();
		s.channels.resize(in.Count(8 + 4 + 4 + 8 + 8));
		for (AnimationChannel& c : s.channels) {
			c.node = static_cast<std::size_t>(in.Value<std::uint64_t>());
			c.path = in.Enum(AnimationPath::Scale);
			c.interpolation = in.Enum(Interpolation::CubicSpline);
			c.times = in.Array<float>();
			c.values = in.Array<DirectX::XMFLOAT4>();
		}
		s.duration = in.Value<float>();
		s.palette = in.Array<PaletteEntry>();
		s.normalize = in.Value<DirectX::XMFLOAT4X4>();
		return s;
	}

	void WriteHumanBones(Writer& out, const HumanBones& bones) {
		out.Count(bones.size());
		for (const auto& [name, node] : bones) {
			out.String(name);
			out.Value<std::uint64_t>(node);
		}
	}

	HumanBones ReadHumanBones(Reader& in) {
		HumanBones bones;
		const std::size_t count = in.Count(8 + 8);
		for (std::size_t i = 0; i < count; ++i) {
			std::wstring name = in.String();
			bones[std::move(name)] = static_cast<std::size_t>(in.Value<std::uint64_t>());
		}
		return bones;
	}

	// ---- 材質（MToon） ----
	void WriteMToon(Writer& out, const MToonMaterial& m) {
		out.Value(m.shadeColor);
		out.Index(m.shadeMultiplyTexture);
		out.Value(m.shadingShift);
		out.Index(m.shadingShiftTexture);
		out.Value(m.shadingShiftTextureScale);
		out.Value(m.shadingToony);
		out.Value(m.giEqualization);
		out.Value(m.matcapColor);
		out.Index(m.matcapTexture);
		out.Value(m.rimColor);
		out.Value(m.rimFresnelPower);
		out.Value(m.rimLift);
		out.Index(m.rimMultiplyTexture);
		out.Value(m.rimLightingMix);
		out.Value(static_cast<std::uint32_t>(m.outlineWidthMode));
		out.Value(m.outlineWidth);
		out.Index(m.outlineWidthMultiplyTexture);
		out.Value(m.outlineColor);
		out.Value(m.outlineLightingMix);
		out.Value<std::int32_t>(m.renderQueueOffset);
	}

	MToonMaterial ReadMToon(Reader& in) {
		MToonMaterial m;
		m.shadeColor = in.Value<DirectX::XMFLOAT3>();
		m.shadeMultiplyTexture = in.Index();
		m.shadingShift = in.Value<float>();
		m.shadingShiftTexture = in.Index();
		m.shadingShiftTextureScale = in.Value<float>();
		m.shadingToony = in.Value<float>();
		m.giEqualization = in.Value<float>();
		m.matcapColor = in.Value<DirectX::XMFLOAT3>();
		m.matcapTexture = in.Index();
		m.rimColor = in.Value<DirectX::XMFLOAT3>();
		m.rimFresnelPower = in.Value<float>();
		m.rimLift = in.Value<float>();
		m.rimMultiplyTexture = in.Index();
		m.rimLightingMix = in.Value<float>();
		m.outlineWidthMode = in.Enum(OutlineWidthMode::ScreenCoordinates);
		m.outlineWidth = in.Value<float>();
		m.outlineWidthMultiplyTexture = in.Index();
		m.outlineColor = in.Value<DirectX::XMFLOAT3>();
		m.outlineLightingMix = in.Value<float>();
		m.renderQueueOffset = in.Value<std::int32_t>();
		return m;
	}

	// ---- 揺れもの ----
	void WriteSpringBones(Writer& out, const SpringBoneData& data) {
		out.Count(data.colliders.size());
		for (const SpringCollider& c : data.colliders) {
			out.Value<std::uint64_t>(c.node);
			// どちらの形かを 0 / 1 で書いてから、その形の中身（std::variant の index と get_if）
			out.Value<std::uint32_t>(static_cast<std::uint32_t>(c.shape.index()));
			if (const auto* sphere = std::get_if<SphereShape>(&c.shape)) out.Value(*sphere);
			if (const auto* capsule = std::get_if<CapsuleShape>(&c.shape)) out.Value(*capsule);
		}
		out.Count(data.colliderGroups.size());
		for (const auto& group : data.colliderGroups) out.Array(group);
		out.Count(data.springs.size());
		for (const Spring& spring : data.springs) {
			out.String(spring.name);
			out.Array(spring.joints);
			out.Array(spring.colliderGroups);
		}
	}

	SpringBoneData ReadSpringBones(Reader& in) {
		SpringBoneData data;
		data.colliders.resize(in.Count(8 + 4));
		for (SpringCollider& c : data.colliders) {
			c.node = static_cast<std::size_t>(in.Value<std::uint64_t>());
			const auto kind = in.Value<std::uint32_t>();
			if (kind == 0) c.shape = in.Value<SphereShape>();
			else if (kind == 1) c.shape = in.Value<CapsuleShape>();
			else THROW_HR_MSG(kInvalidData, "an unknown collider shape");
		}
		data.colliderGroups.resize(in.Count(8));
		for (auto& group : data.colliderGroups) group = in.Array<std::size_t>();
		data.springs.resize(in.Count(8 + 8 + 8));
		for (Spring& spring : data.springs) {
			spring.name = in.String();
			spring.joints = in.Array<SpringJoint>();
			spring.colliderGroups = in.Array<std::size_t>();
		}
		return data;
	}

	// ---- 確かめる ----
	void Check(bool ok, const char* what) {
		THROW_HR_IF_MSG(kInvalidData, !ok, "invalid asset: %hs", what);
	}

	bool InRange(const std::optional<std::size_t>& index, std::size_t size) { return !index || *index < size; }

	void ValidateSkeleton(const Skeleton& s) {
		const std::size_t nodes = s.parents.size();
		Check(s.rest.size() == nodes && s.order.size() == nodes, "node counts differ");
		for (std::size_t i = 0; i < nodes; ++i) {
			Check(s.parents[i] >= -1 && s.parents[i] < static_cast<int>(nodes) && s.parents[i] != static_cast<int>(i), "a parent out of range");
		}
		// order は 0〜nodes-1 をちょうど 1 回ずつ（親から順に計算するときに、どのノードも 1 回だけ触る）
		std::vector<bool> seen(nodes, false);
		for (std::size_t node : s.order) {
			Check(node < nodes && !seen[node], "order is not a permutation");
			seen[node] = true;
		}
		for (const AnimationChannel& c : s.channels) {
			Check(c.node < nodes, "an animation node out of range");
			Check(!c.times.empty(), "an animation without keyframes");
			// キーフレームは時刻の順（FindKeyframe は並んでいる前提で二分探索する）
			Check(std::ranges::all_of(c.times, [](float t) { return std::isfinite(t); })
				&& std::ranges::is_sorted(c.times), "animation times are not finite and sorted");
			const std::size_t perTime = c.interpolation == Interpolation::CubicSpline ? 3 : 1;
			Check(c.values.size() == c.times.size() * perTime, "animation values do not match times");
		}
		Check(std::isfinite(s.duration) && s.duration >= 0.0f, "a bad duration");
		Check(s.palette.size() <= kMaxJoints, "too many joints");
		for (const PaletteEntry& p : s.palette) Check(p.node < nodes, "a joint node out of range");
	}

	void ValidateHumanBones(const HumanBones& bones, std::size_t nodes) {
		for (const auto& [name, node] : bones) Check(node < nodes, "a humanoid bone out of range");
	}
}

namespace rescue::mascot {

	void ValidateMesh(const MeshData& mesh) {
		ValidateSkeleton(mesh.skeleton);
		const std::size_t nodes = mesh.skeleton.parents.size();
		const std::size_t vertices = mesh.vertices.size();
		const std::size_t images = mesh.images.size();

		Check(mesh.indices.size() % 3 == 0, "indices are not triangles");
		for (std::uint32_t index : mesh.indices) Check(index < vertices, "an index out of range");
		for (const MeshVertex& v : mesh.vertices) {
			Check(v.joints.x < kMaxJoints && v.joints.y < kMaxJoints && v.joints.z < kMaxJoints && v.joints.w < kMaxJoints, "a vertex joint out of range");
		}
		for (const MeshPart& part : mesh.parts) {
			Check(static_cast<std::uint64_t>(part.firstIndex) + part.indexCount <= mesh.indices.size(), "a part out of range");
			Check(InRange(part.image, images) && InRange(part.emissiveImage, images), "a part image out of range");
			if (part.mtoon) {
				const MToonMaterial& m = *part.mtoon;
				Check(InRange(m.shadeMultiplyTexture, images) && InRange(m.shadingShiftTexture, images) && InRange(m.matcapTexture, images)
					&& InRange(m.rimMultiplyTexture, images) && InRange(m.outlineWidthMultiplyTexture, images), "an MToon image out of range");
			}
		}
		for (const DecodedImage& image : mesh.images) {
			const bool empty = image.width == 0 || image.height == 0;
			Check(empty ? image.pixels.empty()
				: image.width <= kMaxImageSide && image.height <= kMaxImageSide
					&& image.pixels.size() == static_cast<std::size_t>(image.width) * image.height * 4, "an image size does not match its pixels");
		}
		for (const MorphTarget& target : mesh.morphTargets) {
			for (const MorphDelta& delta : target.deltas) Check(delta.vertex < vertices, "a morph vertex out of range");
		}
		for (const MeshExpression& expression : mesh.expressions) {
			for (const MeshExpression::Morph& morph : expression.morphs) Check(morph.target < mesh.morphTargets.size(), "an expression target out of range");
		}
		const SpringBoneData& springs = mesh.springBones;
		for (const SpringCollider& c : springs.colliders) Check(c.node < nodes, "a collider node out of range");
		for (const auto& group : springs.colliderGroups) {
			for (std::size_t c : group) Check(c < springs.colliders.size(), "a collider group out of range");
		}
		for (const Spring& spring : springs.springs) {
			for (const SpringJoint& joint : spring.joints) Check(joint.node < nodes, "a spring joint out of range");
			for (std::size_t group : spring.colliderGroups) Check(group < springs.colliderGroups.size(), "a spring collider group out of range");
		}
		ValidateHumanBones(mesh.humanBones, nodes);
		Check(std::isfinite(mesh.radius) && mesh.radius > 0.0f, "a bad radius");
	}

	void ValidateMotion(const MotionData& motion) {
		ValidateSkeleton(motion.skeleton);
		ValidateHumanBones(motion.humanBones, motion.skeleton.parents.size());
		for (const MotionExpression& e : motion.expressions) Check(e.node < motion.skeleton.parents.size(), "an expression node out of range");
	}

	void SaveModelAsset(const MeshData& mesh, const std::filesystem::path& path) {
		ValidateMesh(mesh);
		std::filesystem::path temporary = path;
		temporary += L".tmp";
		{
			Writer out{ temporary };
			WriteHeader(out, config::kMascotModelMagic);
			out.Array(mesh.vertices);
			out.Array(mesh.indices);
			out.Count(mesh.parts.size());
			for (const MeshPart& part : mesh.parts) {
				out.Value(part.firstIndex);
				out.Value(part.indexCount);
				out.Index(part.image);
				out.Value(static_cast<std::uint32_t>(part.alphaMode));
				out.Value(part.alphaCutoff);
				out.Value(part.alpha);
				out.Bool(part.doubleSided);
				out.Value(part.emissive);
				out.Index(part.emissiveImage);
				out.Bool(part.mtoon.has_value());
				if (part.mtoon) WriteMToon(out, *part.mtoon);
			}
			out.Count(mesh.images.size());
			for (const DecodedImage& image : mesh.images) {
				out.Value<std::uint32_t>(image.width);
				out.Value<std::uint32_t>(image.height);
				out.Array(image.pixels);
			}
			WriteSkeleton(out, mesh.skeleton);
			out.Count(mesh.morphTargets.size());
			for (const MorphTarget& target : mesh.morphTargets) out.Array(target.deltas);
			out.Count(mesh.expressions.size());
			for (const MeshExpression& expression : mesh.expressions) {
				out.String(expression.name);
				out.Bool(expression.isBinary);
				out.Array(expression.morphs);
			}
			WriteSpringBones(out, mesh.springBones);
			WriteHumanBones(out, mesh.humanBones);
			out.String(mesh.info);
			out.Value(mesh.radius);
			out.Finish();
		}
		std::filesystem::rename(temporary, path);	// 同じフォルダなので置き換え（前のファイルがあっても上書き）
	}

	MeshData LoadModelAsset(const std::filesystem::path& path) {
		Reader in{ path };
		ReadHeader(in, config::kMascotModelMagic);
		MeshData mesh;
		mesh.vertices = in.Array<MeshVertex>();
		mesh.indices = in.Array<std::uint32_t>();
		mesh.parts.resize(in.Count(4 + 4 + 8 + 4 + 4 + 4 + 1 + 12 + 8 + 1));
		for (MeshPart& part : mesh.parts) {
			part.firstIndex = in.Value<std::uint32_t>();
			part.indexCount = in.Value<std::uint32_t>();
			part.image = in.Index();
			part.alphaMode = in.Enum(AlphaMode::Blend);
			part.alphaCutoff = in.Value<float>();
			part.alpha = in.Value<float>();
			part.doubleSided = in.Bool();
			part.emissive = in.Value<DirectX::XMFLOAT3>();
			part.emissiveImage = in.Index();
			if (in.Bool()) part.mtoon = ReadMToon(in);
		}
		mesh.images.resize(in.Count(4 + 4 + 8));
		for (DecodedImage& image : mesh.images) {
			image.width = in.Value<std::uint32_t>();
			image.height = in.Value<std::uint32_t>();
			image.pixels = in.Array<std::byte>();
		}
		mesh.skeleton = ReadSkeleton(in);
		mesh.morphTargets.resize(in.Count(8));
		for (MorphTarget& target : mesh.morphTargets) target.deltas = in.Array<MorphDelta>();
		mesh.expressions.resize(in.Count(8 + 1 + 8));
		for (MeshExpression& expression : mesh.expressions) {
			expression.name = in.String();
			expression.isBinary = in.Bool();
			expression.morphs = in.Array<MeshExpression::Morph>();
		}
		mesh.springBones = ReadSpringBones(in);
		mesh.humanBones = ReadHumanBones(in);
		mesh.info = in.String();
		mesh.radius = in.Value<float>();
		in.ExpectEnd();
		ValidateMesh(mesh);
		return mesh;
	}

	void SaveMotionAsset(const MotionData& motion, const std::filesystem::path& path) {
		ValidateMotion(motion);
		std::filesystem::path temporary = path;
		temporary += L".tmp";
		{
			Writer out{ temporary };
			WriteHeader(out, config::kMascotMotionMagic);
			WriteSkeleton(out, motion.skeleton);
			WriteHumanBones(out, motion.humanBones);
			out.Count(motion.expressions.size());	// （版 2 から）
			for (const MotionExpression& e : motion.expressions) {
				out.String(e.name);
				out.Value<std::uint64_t>(e.node);
			}
			out.Finish();
		}
		std::filesystem::rename(temporary, path);
	}

	MotionData LoadMotionAsset(const std::filesystem::path& path) {
		Reader in{ path };
		ReadHeader(in, config::kMascotMotionMagic);
		MotionData motion;
		motion.skeleton = ReadSkeleton(in);
		motion.humanBones = ReadHumanBones(in);
		motion.expressions.resize(in.Count(8 + 8));
		for (MotionExpression& e : motion.expressions) {
			e.name = in.String();
			e.node = static_cast<std::size_t>(in.Value<std::uint64_t>());
		}
		in.ExpectEnd();
		ValidateMotion(motion);
		return motion;
	}
}
