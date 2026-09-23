#pragma once

#include "World/Core/UUID.h"
#include "World/Core/Memory/Memory.h"
#include "World/Core/Memory/PoolAllocator.h"
#include "World/Scene/Entity.h"
#include "World/Scene/SceneCamera.h"
#include "World/Scene/ScriptableEntity.h"
#include "World/Script/ScriptRef.h"
#include "World/Renderer/Texture.h"
#include "World/Schema/Schema.h"
#include "World/Schema/BuiltinAssetOps.h"

#include <any>
#include <box2d/id.h>
#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <unordered_map>

namespace World
{
	// 复制组件时只保留"编辑配置",剔除运行态。默认整体复制;有运行态的组件提供特化。
	template <typename T>
	T CloneComponentConfiguration(const T& source) { return source; }

	struct UUIDComponent
	{
		UUID ID;

		UUIDComponent() = default;
		UUIDComponent(const UUID& id) : ID(id) {}

		WE_SCHEMA_BODY(World, UUIDComponent, Component)
			WE_SCHEMA_META(Category("Scene"),
				Core(),
				Doc("Persistence identity written into the .wd file so entities can be matched across save and load; every entity is expected to carry one."))
			WE_FIELD(ID, Object, Of(UUID));
		WE_SCHEMA_END
	};

	struct TagComponent
	{
		std::string Tag;

		TagComponent() = default;
		TagComponent(const std::string& tag) : Tag(tag) {}

		WE_SCHEMA_BODY(World, TagComponent, Component)
			WE_SCHEMA_META(Category("Scene"),
				Core(),
				Doc("Human-readable entity label used by the hierarchy, the AI command channel and log messages; not an identifier."))
			WE_FIELD(Tag, String);
		WE_SCHEMA_END
	};

	struct TransformComponent
	{
		glm::vec3 Location { 0.0f, 0.0f, 0.0f };
		glm::vec3 Rotation { 0.0f, 0.0f, 0.0f };
		glm::quat RotationQuat { 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale { 1.0f, 1.0f, 1.0f };
		glm::mat4 Transform { 1.0f };

		TransformComponent(const glm::mat4& transform) { SetTransform(transform); }
		TransformComponent(const glm::vec3& location = glm::vec3 { 0.0f, 0.0f, 0.0f }, const glm::vec3& rotation = glm::vec3 { 0.0f, 0.0f, 0.0f }, const glm::vec3& scale = glm::vec3 { 1.0f, 1.0f, 1.0f })
		{
			SetTransform(location, rotation, scale);
		}

		void SetLocation(const glm::vec3& location) { Location = location; RecalculateTransform(); }
		void SetRotation(const glm::vec3& rotation) { Rotation = rotation; RotationQuat = glm::quat(rotation); RecalculateTransform(); }
		void SetScale(const glm::vec3& scale) { Scale = scale; RecalculateTransform(); }
		void SetTransform(const glm::vec3& location, const glm::vec3& rotation, const glm::vec3& scale)
		{
			Location = location;
			Rotation = rotation;
			Scale = scale;
			RotationQuat = glm::quat(rotation);
			RecalculateTransform();
		}
		void SetTransform(const glm::mat4& transform)
		{
			Transform = transform;
			Location = glm::vec3(transform[3]);
			Scale.x = glm::length(glm::vec3(transform[0]));
			Scale.y = glm::length(glm::vec3(transform[1]));
			Scale.z = glm::length(glm::vec3(transform[2]));
			if (glm::determinant(transform) < 0)
				Scale.x *= -1.0f;
			glm::mat4 rotationMatrix = transform;
			rotationMatrix[0] /= Scale.x;
			rotationMatrix[1] /= Scale.y;
			rotationMatrix[2] /= Scale.z;
			rotationMatrix[3] = glm::vec4(0, 0, 0, 1);
			RotationQuat = glm::quat_cast(rotationMatrix);
			Rotation = glm::eulerAngles(RotationQuat);
		}
		void SetRotationQuat(const glm::quat& rotationQuat)
		{
			RotationQuat = rotationQuat;
			Rotation = glm::eulerAngles(rotationQuat);
			RecalculateTransform();
		}
		void RecalculateTransform()
		{
			glm::mat4 rotationMatrix = glm::mat4_cast(RotationQuat);
			glm::mat4 scaleMatrix = glm::scale(glm::mat4(1.0f), Scale);
			glm::mat4 translationMatrix = glm::translate(glm::mat4(1.0f), Location);
			Transform = translationMatrix * rotationMatrix * scaleMatrix;
		}

		operator const glm::mat4&() const { return Transform; }

		WE_SCHEMA_BODY(World, TransformComponent, Component)
			WE_SCHEMA_META(Category("Scene"),
				Core(),
				Doc("Local translation/rotation/scale of the entity; RotationQuat and the cached Transform matrix are derived runtime state (Transient, never serialized)."))
			WE_FIELD(Location, Vec3, Group("Transform"),
				Doc("Local position in the parent's space (world units)."));
			WE_FIELD(Rotation, Vec3, Group("Transform"),
				Doc("Local rotation in degrees (Euler XYZ); the engine stores the equivalent quaternion."));
			WE_FIELD(RotationQuat, Quat, Transient, Group("Transform"));
			WE_FIELD(Scale, Vec3, Group("Transform"),
				Doc("Local scale per axis; 1,1,1 = unscaled, negative values mirror."));
			WE_FIELD(Transform, Mat4, Transient, Group("Transform"));
		WE_SCHEMA_END
	};

	struct SpriteComponent
	{
		glm::vec4 Color { 1.0f, 1.0f, 1.0f, 1.0f };
		Ref<Texture2D> Texture;
		float TilingFactor = 1.0f;

		SpriteComponent() = default;
		SpriteComponent(const glm::vec4& color) : Color(color) {}

		WE_SCHEMA_BODY(World, SpriteComponent, Component)
			WE_SCHEMA_META(Category("Rendering/Mesh"),
				Doc("2D textured quad: an empty Texture falls back to a flat Color fill and TilingFactor repeats the texture UVs."))
			WE_FIELD(Color, Vec4, Color(),
				Doc("Tint multiplied into the sprite (alpha < 1 blends with what is behind)."));
			WE_FIELD(Texture, Asset, Of("Texture2D"));
			WE_FIELD(TilingFactor, Float,
				Doc("Repeats the texture UVs; 1 = once across the quad."));
		WE_SCHEMA_END
	};

	struct CircleRendererComponent
	{
		glm::vec4 Color { 1.0f, 1.0f, 1.0f, 1.0f };
		float Thickness = 1.0f;
		float Fade = 0.005f;

		WE_SCHEMA_BODY(World, CircleRendererComponent, Component)
			WE_SCHEMA_META(Category("Rendering/Mesh"),
				Doc("Flat 2D disc: Thickness is the ring width as a fraction of the radius (1 = filled) and Fade is the normalized edge softness."))
			WE_FIELD(Color, Vec4, Color(),
				Doc("Tint multiplied into the disc."));
			WE_FIELD(Thickness, Float,
				Doc("Ring width as a fraction of the radius: 1 = filled disc, 0.1 = thin ring."));
			WE_FIELD(Fade, Float,
				Doc("Normalized softness of the edge (0 = hard edge)."));
		WE_SCHEMA_END
	};

	// P1b D2c:3D 网格渲染组件。
	// P2a W3:层级关系(父子 + 是否继承父变换)与世界矩阵缓存。
	// Parent 以 UInt64 存 schema(entt::entity 是 32 位句柄),Children 由读档后按 Parent 重建,
	// WorldTransformComponent 是缓存不参与序列化。
	struct HierarchyComponent
	{
		entt::entity Parent = entt::null;
		std::vector<entt::entity> Children;
		bool InheritTransform = true;

		WE_SCHEMA_BODY(World, HierarchyComponent, Component)
			WE_SCHEMA_META(Category("Scene"),
				Doc("Parent link plus whether the parent transform is inherited; Children is a runtime cache rebuilt from Parent after loading."))
			// P2 W3a:字段 id 显式钉住(schema-compiler 的公式值会改写这两个 id,
			// 而它们已经写进存档;显式 Id 让生成物与既有存档迁移语义一致,--check 门禁才可能为绿)。
			// Entity32:Parent 在 C++ 侧是 entt::entity(32 位句柄),schema 存 64 位整数。
			WE_FIELD(Parent, UInt64, Id(0x4849455241524331), Entity32,
				Doc("Parent entity; cleared = the entity is a root."));
			WE_FIELD(InheritTransform, Bool, Id(0x4849455241524332), Default(true),
				Doc("When on, this entity's world transform is parent world x local; off = ignore the parent transform."));
		WE_SCHEMA_END
	};

	struct WorldTransformComponent
	{
		glm::mat4 Matrix { 1.0f };
	};

	// Primitive:内置网格名("cube"/"plane");MeshPath 预留给 glTF 导入的模型资产(D5),
	// 届时 Primitive 会升级为资产引用,这里的字段 id 保持不变以便存档迁移。
	struct MeshRendererComponent
	{
		std::string Primitive = "cube";
		glm::vec4 Color { 1.0f, 1.0f, 1.0f, 1.0f };
		std::string MeshPath;
		// D3:材质资产路径(相对项目内容根,形如 materials/steel.wmat)。
		// 空 = 旧行为:用上面的 Color 直接作为基色。
		std::string MaterialPath;
		// D5:.wmodel 有节点树时,选择"第几个 mesh"(节点引用 mesh 下标);
		// 内置 primitive(cube/plane/sphere)与无 submesh 的网格忽略该字段。
		int32_t MeshIndex = 0;

		WE_SCHEMA_BODY(World, MeshRendererComponent, Component)
			WE_SCHEMA_META(Category("Rendering/Mesh"),
				Doc("Draws a built-in primitive or an imported mesh; MeshIndex selects the mesh inside the model and an empty MaterialPath shades with Color."))
			// 同上:四个字段 id 已随存档/材质资产落盘(D2c/D3 期间手写),显式钉住。
			WE_FIELD(Primitive, String, Id(0x4D4553485052494D), Choices("cube", "sphere", "plane"),
				Doc("Built-in primitive used when MeshPath is empty."));
			WE_FIELD(Color, Vec4, Id(0x4D455348434F4C52), Color(),
				Doc("Base color used when MaterialPath is empty."));
			WE_FIELD(MeshPath, String, Id(0x4D45534850415448), Asset("Model"),
				Doc("Imported model asset (.wmodel, path relative to the project content root); empty = use Primitive. glTF/GLB are import sources only: import them first and reference the produced .wmodel."));
			WE_FIELD(MaterialPath, String, Id(0x4D4154455249414C), Asset("Material"),
				Doc("Material asset (.wmat); overrides Color and the model's own material slots when set."));
			// D5:字段 id 显式钉住("MESHINDX"),默认 0;.wmodel 节点树选择 mesh 用。
			WE_FIELD(MeshIndex, Int32, Id(0x4D455348494E4458), Default(0));
		WE_SCHEMA_END
	};

	// P1b D5c-4a:蒙皮网格渲染组件(骨骼动画)。
	// 路径语义与 MeshRendererComponent 一致:.wmodel 是**相对内容根**的逻辑路径
	// (如 models/rock.wmodel),MeshIndex 选择模型里的第几个 mesh(该 mesh 的 SkinIndex
	// 决定用哪套骨架);MaterialPath 空 = 走模型的材质槽/Color 回退。
	// AnimationClip 空 = 第 0 条 clip;Time 是动画系统每帧写入的运行态(允许进 schema,
	// Play/Simulate 下由 AnimationSystem 写,属性面板只读)。
	// 字段 id 全部显式钉住("SK…" ASCII):一旦写进 .wd 存档就不能再改,否则旧场景迁移语义漂移。
	struct SkinnedMeshRendererComponent
	{
		std::string MeshPath;
		int32_t MeshIndex = 0;
		std::string MaterialPath;
		std::string AnimationClip;
		bool Playing = true;
		float Speed = 1.0f;
		bool Loop = true;
		float Time = 0.0f;

		WE_SCHEMA_BODY(World, SkinnedMeshRendererComponent, Component)
			WE_SCHEMA_META(Category("Rendering/Mesh"),
				Doc("Skeleton-driven mesh: an empty AnimationClip plays the first clip and Time is written by the animation system (read-only under Play)."))
			WE_FIELD(MeshPath, String, Id(0x534B4D4553485041), Asset("Model"),
				Doc("Skinned model asset (.wmodel); required — this component draws nothing without it."));
			WE_FIELD(MeshIndex, Int32, Id(0x534B4D4553484958), Default(0));
			WE_FIELD(MaterialPath, String, Id(0x534B4D4154505448), Asset("Material"),
				Doc("Material asset (.wmat); empty = the model's own material slots."));
			WE_FIELD(AnimationClip, String, Id(0x534B414E494D434C),
				Doc("Clip name inside the model; empty = play the first clip."));
			WE_FIELD(Playing, Bool, Id(0x534B504C4159494E), Default(true),
				Doc("Play the clip automatically."));
			WE_FIELD(Speed, Float, Id(0x534B535045454430), Default(1.0f),
				Doc("Playback speed multiplier (1 = the clip's authored speed)."));
			WE_FIELD(Loop, Bool, Id(0x534B4C4F4F503030), Default(true),
				Doc("Loop the clip when it reaches the end."));
			WE_FIELD(Time, Float, Id(0x534B54494D453030), Default(0.0f));
		WE_SCHEMA_END
	};

	struct CameraComponent
	{
		CameraComponent() = default;
		CameraComponent(const SceneCamera& sceneCamera, bool primary = false)
			: Camera(sceneCamera), Primary(primary)
		{
		}

		SceneCamera Camera;
		bool Primary = true;
		bool FixedAspectRatio = false;

		WE_SCHEMA_BODY(World, CameraComponent, Component)
			WE_SCHEMA_META(Category("Scene"),
				Doc("Scene camera settings plus Primary (the camera Play and the runtime render through); FixedAspectRatio keeps the projection from following the viewport size."))
			WE_FIELD(Camera, Object, Of(SceneCamera));
			WE_FIELD(Primary, Bool,
				Doc("The scene renders through the first primary camera in Play and in the runtime."));
			WE_FIELD(FixedAspectRatio, Bool,
				Doc("Keep the projection aspect from the editor instead of following the viewport/window size."));
		WE_SCHEMA_END
	};

	// P1b D4:灯光组件(前向渲染,每帧打包进全局 set0 的灯光 UBO)。
	// 上限:方向光 1 盏(只取第一盏,阴影也只做它)、点光 7 盏、合计 8 个 UBO 槽位;
	// 超出的按 registry 遍历顺序截断,截断数进 Renderer3D::Statistics 与 [lighting] 日志。
	// Color 是**线性 sRGB 数值**(与材质 BaseColor 同一约定:着色器里 pow(2.2) 解码)。
	struct DirectionalLightComponent
	{
		glm::vec3 Color { 1.0f, 1.0f, 1.0f };
		float Intensity = 1.0f;
		// 光的**传播方向**(从光源指向场景,世界空间;打包时归一化,零向量回退 -Y)。
		glm::vec3 Direction { 0.35f, -0.7f, 0.6f };
		// 主方向光是否跑阴影通道(2048² 深度图,3×3 PCF)。
		bool CastShadow = false;

		WE_SCHEMA_BODY(World, DirectionalLightComponent, Component)
			WE_SCHEMA_META(Category("Rendering/Light"),
				Doc("Sun-style light: Direction is the propagation direction in world space (normalized when packed) and only the first one casts shadows."))
			WE_FIELD(Color, Vec3, Group("Light"), Color(),
				Doc("Linear color of the light (values are decoded with pow 2.2 by the shader)."));
			WE_FIELD(Intensity, Float, Group("Light"), Range(0.0f, 100.0f),
				Doc("Brightness multiplier applied to Color."));
			WE_FIELD(Direction, Vec3, Group("Light"), 
				Doc("Propagation direction in world space (normalized when packed); zero falls back to -Y."));
			WE_FIELD(CastShadow, Bool, Group("Light"),
				Doc("Render a 2048² shadow map with 3x3 PCF; only the first directional light casts shadows."));
		WE_SCHEMA_END
	};

	struct PointLightComponent
	{
		glm::vec3 Color { 1.0f, 1.0f, 1.0f };
		float Intensity = 1.0f;
		// 衰减范围(世界单位):衰减系数 = saturate(1 - d/range)^2。
		float Range = 10.0f;

		WE_SCHEMA_BODY(World, PointLightComponent, Component)
			WE_SCHEMA_META(Category("Rendering/Light"),
				Doc("Local light with distance falloff: Range is the falloff radius in world units, attenuation saturates as (1 - d/Range)^2, and at most 7 point lights reach the shader."))
			WE_FIELD(Color, Vec3, Group("Light"), Color(),
				Doc("Linear color of the light (decoded with pow 2.2 by the shader)."));
			WE_FIELD(Intensity, Float, Group("Light"), Range(0.0f, 100.0f),
				Doc("Brightness multiplier applied to Color."));
			WE_FIELD(Range, Float, Group("Light"), Range(0.0f, 1000.0f),
				Doc("Falloff radius in world units: attenuation is (1 - d/Range)² and reaches zero at Range."));
		WE_SCHEMA_END
	};

	// 场景级环境光:多盏时**第一盏**生效;没有该组件时用 0.25 灰默认值
	// (与 D4 之前的占位实现同观感,既有场景不会突然全黑)。
	struct AmbientLightComponent
	{
		glm::vec3 Color { 1.0f, 1.0f, 1.0f };
		float Intensity = 0.25f;

		WE_SCHEMA_BODY(World, AmbientLightComponent, Component)
			WE_SCHEMA_META(Category("Rendering/Light"),
				Doc("Global ambient term without a spatial range: only the first ambient light is used and 0.25 grey applies when the scene has none."))
			WE_FIELD(Color, Vec3, Group("Light"), Color(),
				Doc("Linear ambient color added to every surface (decoded with pow 2.2)."));
			WE_FIELD(Intensity, Float, Group("Light"), Range(0.0f, 100.0f),
				Doc("Ambient strength: 0 = no ambient, 0.25 is the engine default when the scene has none."));
		WE_SCHEMA_END
	};

	enum class ScriptInstanceState { Pending, Creating, Running, Destroying, Stopped, Faulted };

	struct NativeScriptComponent
	{
		ScriptableEntity* Instance = nullptr;
		ScriptInstanceState State = ScriptInstanceState::Pending;
		std::string LastError;
		uint64_t Generation = 0;
		bool CreateEntered = false;
		ScriptableEntity* (*InstantiateScript)() = nullptr;
		void (*DestroyScript)(ScriptableEntity*&) = nullptr;

		std::string ScriptName;
		std::unordered_map<std::string, Schema::Value> FieldValues;

		template <typename T>
		void Bind()
		{
			InstantiateScript = []()
			{
				return static_cast<ScriptableEntity*>(WLD_POOL_NEW(T));
			};
			DestroyScript = [](ScriptableEntity*& scriptableEntity)
			{
				WLD_POOL_DELETE(T, PoolTag::General, scriptableEntity);
				scriptableEntity = nullptr;
			};
		}

		WE_SCHEMA_BODY(World, NativeScriptComponent, Component)
			WE_SCHEMA_META(Category("Scripting"),
				Doc("C++ behavior instance created per entity when Play starts; ScriptName selects the registered script and the remaining fields are runtime state."))
			WE_FIELD(ScriptName, String);
		WE_SCHEMA_END

		// 与 UI 框架解耦的字段访问合同,供 Editor Inspector 与测试共用;运行期场景也用它应用字段。
		ScriptableEntity* GetOrCreateEditorInstance(bool allowCreate, bool& outOwned);
		void ReleaseEditorInstance(ScriptableEntity* preview);
		void ResetEditorFieldState();
		Schema::Value GetErasedFieldValue(const Schema::TypeSchema& typeSchema, const Schema::FieldSchema& field, ScriptableEntity* instance);
		void SetErasedFieldValue(const Schema::TypeSchema& typeSchema, const Schema::FieldSchema& field, ScriptableEntity* instance, const Schema::Value& value);

	private:
		bool isFirstDraw = true;
	};

	enum class LuaFieldType { None, Float, Int, Bool, String };
	struct LuaScriptField
	{
		LuaFieldType Type = LuaFieldType::None;
		std::any Value;

		static std::string GetLuaTypeName(LuaFieldType type)
		{
			switch (type)
			{
				case LuaFieldType::None: return "any";
				case LuaFieldType::Float: return "number";
				case LuaFieldType::Int: return "number";
				case LuaFieldType::Bool: return "boolean";
				case LuaFieldType::String: return "string";
				default: return "any";
			}
		}
	};

	struct LuaScriptComponent
	{
		std::string ScriptFilePath = ""; // 例如 "assets/scripts/Player.lua"

		// 每个实体独立的 Luau environment,防止变量冲突(W1b 起为绑定层引用)。
		ScriptTableRef LuaEnv;
		ScriptTableRef ScriptTable;
		ScriptFunctionRef OnCreateFunc;
		ScriptFunctionRef OnUpdateFunc;
		ScriptFunctionRef OnDestroyFunc;
		// W3c:UI 阶段的每帧回调(命令式脚本 UI)。宿主在 UI 阶段调用它,
		// 脚本在其中用 ui.* 画控件;热重载时与新表一起整体交换。
		ScriptFunctionRef OnUiFunc;

		std::unordered_map<std::string, LuaScriptField> CachedFields;
		std::filesystem::file_time_type LastModifiedTime;
		// W5:热重载用的源指纹(优先内容哈希,退化为 mtime+size)与最近一次重载诊断。
		// 都是运行期状态,不进 schema、不参与序列化;克隆配置时只带指纹(源文件身份)。
		uint64_t SourceFingerprint = 0;
		std::string ReloadDiagnostic;

		bool IsLoaded = false;
		ScriptInstanceState State = ScriptInstanceState::Pending;
		std::string LastError;
		uint64_t Generation = 0;
		bool CreateEntered = false;
		Entity RuntimeEntity;

		LuaScriptComponent() = default;
		LuaScriptComponent(const LuaScriptComponent&) = default;
		LuaScriptComponent(const std::string& path) : ScriptFilePath(path) {}

		WE_SCHEMA_BODY(World, LuaScriptComponent, Component)
			WE_SCHEMA_META(Category("Scripting"),
				Doc("Luau script attached to the entity: only ScriptFilePath is serialized, the environment, callbacks and cached fields are rebuilt at load."))
			WE_FIELD(ScriptFilePath, String, Asset("Script"),
				Doc("Luau script asset (.luau/.lua) relative to the project content root, e.g. scripts/Player.luau."));
		WE_SCHEMA_END
	};

	struct RigidBody2DComponent
	{
		enum class BodyType
		{
			Static = 0, Dynamic, Kinematic
		};
		WE_ENUM_SCHEMA(World, BodyType, Int32)
			WE_ENUM_VALUE(Static);
			WE_ENUM_VALUE(Dynamic);
			WE_ENUM_VALUE(Kinematic);
		WE_ENUM_END

		BodyType Type = BodyType::Static;
		b2BodyId RuntimeBodyId = b2_nullBodyId;
		bool FixedRotation = false;

		WE_SCHEMA_BODY(World, RigidBody2DComponent, Component)
			WE_SCHEMA_META(Category("Physics/2D"),
				Doc("Box2D body: BodyType selects Static/Dynamic/Kinematic and FixedRotation locks the angular degree of freedom."))
			WE_FIELD(Type, Enum, Of(BodyType),
				Doc("Static never moves, Dynamic is driven by forces/gravity, Kinematic moves only through code."));
			WE_FIELD(FixedRotation, Bool,
				Doc("Lock the angular degree of freedom so collisions cannot rotate the body."));
		WE_SCHEMA_END
	};

	struct BoxCollider2DComponent
	{
		glm::vec2 Offset { 0.0f, 0.0f };
		glm::vec2 Size { 0.5f, 0.5f };
		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.2f;
		bool ShowCollider = true;

		WE_SCHEMA_BODY(World, BoxCollider2DComponent, Component)
			WE_SCHEMA_META(Category("Physics/2D"),
				Doc("2D box collider simulated by Box2D: Size defines the box in local units (multiplied by the entity transform) and Offset is the local centre offset."))
			WE_FIELD(Offset, Vec2,
				Doc("Centre of the box in local units (scaled by the entity transform)."));
			WE_FIELD(Size, Vec2,
				Doc("Half-extents of the box in local units (scaled by the entity transform)."));
			WE_FIELD(Density, Float,
				Doc("Mass per area: the body mass is density x collider area."));
			WE_FIELD(Friction, Float,
				Doc("Surface friction against other colliders (0 = ice, 1 = rough)."));
			WE_FIELD(Restitution, Float,
				Doc("Bounciness: 0 = no bounce, 1 = perfectly elastic."));
			WE_FIELD(ShowCollider, Bool,
				Doc("Draw the Box2D debug outline for this collider while simulating."));
		WE_SCHEMA_END
	};

	struct CircleCollider2DComponent
	{
		glm::vec2 Offset { 0.0f, 0.0f };
		float Radius = 0.5f;
		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.2f;
		bool ShowCollider = true;

		WE_SCHEMA_BODY(World, CircleCollider2DComponent, Component)
			WE_SCHEMA_META(Category("Physics/2D"),
				Doc("2D circle collider simulated by Box2D: Radius scaled by the entity transform with a local Offset; ShowCollider toggles the physics debug outline."))
			WE_FIELD(Offset, Vec2,
				Doc("Centre of the circle in local units (scaled by the entity transform)."));
			WE_FIELD(Radius, Float,
				Doc("Radius in local units (scaled by the entity transform)."));
			WE_FIELD(Density, Float,
				Doc("Mass per area: the body mass is density x circle area."));
			WE_FIELD(Friction, Float,
				Doc("Surface friction against other colliders (0 = ice, 1 = rough)."));
			WE_FIELD(Restitution, Float,
				Doc("Bounciness: 0 = no bounce, 1 = perfectly elastic."));
			WE_FIELD(ShowCollider, Bool,
				Doc("Draw the Box2D debug outline for this collider while simulating."));
		WE_SCHEMA_END
	};

	// P1b D6:3D 物理(Jolt)组件。字段 id 全部显式钉住("RB3D…"/"…3D…" ASCII):
	// 组件一旦写进 .wd 存档就不能再改 id,否则旧场景迁移语义会漂移,--check 门禁也要求
	// 生成物与注解逐字节一致。
	struct RigidBody3DComponent
	{
		// 枚举名必须模块内唯一(schema-compiler 的 WE_ENUM_SCHEMA 只接受单个标识符,
		// 不能写成 RigidBody3DComponent::BodyType);2D 已经占用了 BodyType。
		enum class MotionType
		{
			Static = 0, Kinematic, Dynamic
		};
		WE_ENUM_SCHEMA(World, MotionType, Int32)
			WE_ENUM_VALUE(Static);
			WE_ENUM_VALUE(Kinematic);
			WE_ENUM_VALUE(Dynamic);
		WE_ENUM_END

		// 默认 Static(与 RigidBody2DComponent 同口径:挂上不会自己掉下去)。
		MotionType Type = MotionType::Static;
		// Dynamic 刚体的质量(由形状质量属性折算惯量);Static/Kinematic 忽略。
		float Mass = 1.0f;
		float LinearDamping = 0.0f;
		float AngularDamping = 0.0f;
		float Friction = 0.5f;
		float Restitution = 0.2f;
		bool UseGravity = true;

		WE_SCHEMA_BODY(World, RigidBody3DComponent, Component)
			WE_SCHEMA_META(Category("Physics/3D"),
				Doc("Jolt body: Mass and the damping terms drive Dynamic bodies, UseGravity opts into scene gravity, and Static bodies never move."))
			WE_FIELD(Type, Enum, Id(0x5242334454595045), Of(MotionType),
				Doc("Static never moves, Kinematic moves only through code, Dynamic follows forces and gravity."));
			WE_FIELD(Mass, Float, Id(0x524233444D415353), Range(0.0f, 100000.0f),
				Doc("Mass in kilograms used for Dynamic bodies (the shape only contributes inertia)."));
			WE_FIELD(LinearDamping, Float, Id(0x524233444C4E4450), Range(0.0f, 100.0f),
				Doc("Velocity damping per second: higher values stop a sliding body faster."));
			WE_FIELD(AngularDamping, Float, Id(0x52423344414E4744), Range(0.0f, 100.0f),
				Doc("Spin damping per second: higher values stop a rotating body faster."));
			WE_FIELD(Friction, Float, Id(0x5242334446524943), Range(0.0f, 1.0f),
				Doc("Surface friction against other bodies (0 = ice, 1 = rough)."));
			WE_FIELD(Restitution, Float, Id(0x5242334452455354), Range(0.0f, 1.0f),
				Doc("Bounciness: 0 = no bounce, 1 = perfectly elastic."));
			WE_FIELD(UseGravity, Bool, Id(0x5242334447525654),
				Doc("Apply the scene gravity to this body (see Project Settings ▸ Physics)."));
		WE_SCHEMA_END
	};

	// 盒体碰撞:HalfExtents 是**局部半尺寸**,随 TransformComponent.Scale 缩放;Offset 是局部偏移(不缩放)。
	struct BoxCollider3DComponent
	{
		glm::vec3 HalfExtents { 0.5f, 0.5f, 0.5f };
		glm::vec3 Offset { 0.0f, 0.0f, 0.0f };

		WE_SCHEMA_BODY(World, BoxCollider3DComponent, Component)
			WE_SCHEMA_META(Category("Physics/3D"),
				Doc("Box shape: HalfExtents are local half sizes scaled by the entity transform and Offset is a local offset applied unscaled."))
			WE_FIELD(HalfExtents, Vec3, Id(0x42334448414C4658),
				Doc("Half size of the box in local units (scaled by the entity transform)."));
			WE_FIELD(Offset, Vec3, Id(0x4233444F46465354),
				Doc("Shape centre offset in local units (not scaled by the entity transform)."));
		WE_SCHEMA_END
	};

	struct SphereCollider3DComponent
	{
		float Radius = 0.5f;
		glm::vec3 Offset { 0.0f, 0.0f, 0.0f };

		WE_SCHEMA_BODY(World, SphereCollider3DComponent, Component)
			WE_SCHEMA_META(Category("Physics/3D"),
				Doc("Sphere shape: Radius in local units with a local Offset applied unscaled."))
			WE_FIELD(Radius, Float, Id(0x5333445241444955), Range(0.0f, 100000.0f),
				Doc("Sphere radius in local units (scaled by the entity transform)."));
			WE_FIELD(Offset, Vec3, Id(0x5333444F46465354),
				Doc("Shape centre offset in local units (not scaled by the entity transform)."));
		WE_SCHEMA_END
	};

	// 胶囊:轴沿实体的局部 Y 轴(与 Jolt CapsuleShape 的约定一致),HalfHeight 是圆柱段半高(不含两端半球)。
	struct CapsuleCollider3DComponent
	{
		float Radius = 0.5f;
		float HalfHeight = 0.5f;
		glm::vec3 Offset { 0.0f, 0.0f, 0.0f };

		WE_SCHEMA_BODY(World, CapsuleCollider3DComponent, Component)
			WE_SCHEMA_META(Category("Physics/3D"),
				Doc("Capsule along the entity's local Y axis: HalfHeight is the cylinder half height excluding the two hemisphere caps."))
			WE_FIELD(Radius, Float, Id(0x4333445241444955), Range(0.0f, 100000.0f),
				Doc("Radius of the capsule and its two hemisphere caps."));
			WE_FIELD(HalfHeight, Float, Id(0x43334448414C4648), Range(0.0f, 100000.0f),
				Doc("Half height of the cylinder segment (excludes the hemisphere caps)."));
			WE_FIELD(Offset, Vec3, Id(0x4333444F46465354),
				Doc("Shape centre offset in local units (not scaled by the entity transform)."));
		WE_SCHEMA_END
	};

	// 网格碰撞:MeshPath 空 = 用同实体 MeshRendererComponent.MeshPath(相对内容根的 .wmodel 路径);
	// ConvexHull 可挂 Static/Kinematic/Dynamic,StaticTriangles 只允许 Static 刚体(运行时拒绝)。
	struct MeshCollider3DComponent
	{
		// 枚举名不能叫 Mode:字段也叫 Mode,同名成员变量会遮蔽嵌套类型名
		// (生成代码里的 static_cast<MeshCollider3DComponent::Mode> 会解析成非静态成员)。
		enum class ColliderMode
		{
			ConvexHull = 0, StaticTriangles
		};
		WE_ENUM_SCHEMA(World, ColliderMode, Int32)
			WE_ENUM_VALUE(ConvexHull);
			WE_ENUM_VALUE(StaticTriangles);
		WE_ENUM_END

		ColliderMode Mode = ColliderMode::ConvexHull;
		std::string MeshPath;

		WE_SCHEMA_BODY(World, MeshCollider3DComponent, Component)
			WE_SCHEMA_META(Category("Physics/3D"),
				Doc("Mesh-derived collision: an empty MeshPath uses the entity's MeshRenderer mesh, StaticTriangles only works on Static bodies, and the editor draws no outline for it."))
			WE_FIELD(Mode, Enum, Id(0x4D33444D4F444530), Of(ColliderMode),
				Doc("ConvexHull works on every body type; StaticTriangles is only allowed on Static bodies."));
			WE_FIELD(MeshPath, String, Id(0x4D33445041544830), Asset("Model"),
				Doc("Collision model asset (.wmodel); empty = reuse the MeshRenderer mesh on the same entity. glTF/GLB sources must be imported to .wmodel first."));
		WE_SCHEMA_END
	};

	// 组件配置克隆特化(剔除运行态)。
	NativeScriptComponent CloneComponentConfiguration(const NativeScriptComponent& source);
	LuaScriptComponent CloneComponentConfiguration(const LuaScriptComponent& source);
	RigidBody2DComponent CloneComponentConfiguration(const RigidBody2DComponent& source);
}
