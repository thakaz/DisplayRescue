#include "mascot/SpringBone.h"

#include "mascot/JsonRead.h"	// C++/WinRT の JSON（Windows.Data.Json）と、数・3 つ組を読む関数

#pragma comment(lib,"windowsapp.lib")

namespace {
	using winrt::Windows::Data::Json::IJsonValue;
	using winrt::Windows::Data::Json::JsonObject;
	using namespace rescue::mascot;
	using json::Number;
	using json::Vector3;

	// 1. 番号の配列（[0, 3, 5] のような、ほかの配列の添字の並び）。無ければ空
	std::vector<std::size_t> Indices(const JsonObject& object, std::wstring_view name) {
		std::vector<std::size_t> indices;
		if (!object.HasKey(name)) return indices;
		for (const IJsonValue& value : object.GetNamedArray(name)) {
			indices.push_back(static_cast<std::size_t>(value.GetNumber()));
		}
		return indices;
	}

	// 2. コライダー（当たり判定）1 つ。形は球かカプセルのどちらか（std::variant に入れる）
	SpringCollider ReadCollider(const JsonObject& object) {
		SpringCollider collider;
		collider.node = static_cast<std::size_t>(object.GetNamedNumber(L"node"));
		const JsonObject shape = object.GetNamedObject(L"shape");
		if (shape.HasKey(L"capsule")) {
			const JsonObject capsule = shape.GetNamedObject(L"capsule");
			collider.shape = CapsuleShape{
				Vector3(capsule, L"offset",{ 0,0,0 }),
				Number(capsule,L"radius",0.0f),
				Vector3(capsule,L"tail",{ 0,0,0 }),
			};
		}
		else {
			const JsonObject sphere = shape.GetNamedObject(L"sphere");
			collider.shape = SphereShape{
				Vector3(sphere,L"offset",{ 0,0,0 }),
				Number(sphere,L"radius",0.0f),
			};
		}
		return collider;
	}

	// 3. ジョイント（揺れる骨）1 つ。無い項目は SpringJoint に書いてある既定値のまま
	SpringJoint ReadJoint(const JsonObject& object) {
		SpringJoint j;
		j.node = static_cast<std::size_t>(object.GetNamedNumber(L"node"));
		j.hitRadius = Number(object,L"hitRadius", j.hitRadius);
		j.stiffness = Number(object, L"stiffness", j.stiffness);
		j.gravityPower = Number(object, L"gravityPower", j.gravityPower);
		j.gravityDir = Vector3(object, L"gravityDir", j.gravityDir);
		j.dragForce = Number(object, L"dragForce", j.dragForce);
		return j;
	}

	// 4. スプリング（根元から先へ並んだジョイントの鎖）1 本
	Spring ReadSpring(const JsonObject& object) {
		Spring spring;
		spring.name = object.GetNamedString(L"name", L"");
		for (const IJsonValue& value : object.GetNamedArray(L"joints")) {
			spring.joints.push_back(ReadJoint(value.GetObject()));
		}
		spring.colliderGroups = Indices(object, L"colliderGroups");
		return spring;
	}
}

namespace rescue::mascot {

	// 5. 全体。3 つの配列（colliders / colliderGroups / springs）を順に回す
	SpringBoneData ParseSpringBones(std::string_view json) {
		const JsonObject root = JsonObject::Parse(winrt::to_hstring(json));
		SpringBoneData data;
		if (root.HasKey(L"colliders")) {
			for (const IJsonValue& value : root.GetNamedArray(L"colliders")) {
				data.colliders.push_back(ReadCollider(value.GetObject()));
			}
		}
		if (root.HasKey(L"colliderGroups")) {
			for (const IJsonValue& value : root.GetNamedArray(L"colliderGroups")) {
				data.colliderGroups.push_back(Indices(value.GetObject(), L"colliders"));
			}
		}
		if (root.HasKey(L"springs")) {
			for (const IJsonValue& value : root.GetNamedArray(L"springs")) {
				data.springs.push_back(ReadSpring(value.GetObject()));
			}
		}
		return data;
	}
}