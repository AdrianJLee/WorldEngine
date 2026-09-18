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
			WE_FIELD(ID, Object, Of(UUID));
		WE_SCHEMA_END
	};

	struct TagComponent
	{
		std::string Tag;

		TagComponent() = default;
		TagComponent(const std::string& tag) : Tag(tag) {}

		WE_SCHEMA_BODY(World, TagComponent, Component)
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
			WE_FIELD(Location, Vec3, Group("Transform"));
			WE_FIELD(Rotation, Vec3, Group("Transform"));
			WE_FIELD(RotationQuat, Quat, Transient, Group("Transform"));
			WE_FIELD(Scale, Vec3, Group("Transform"));
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
			WE_FIELD(Color, Vec4);
			WE_FIELD(Texture, Asset, Of("Texture2D"));
			WE_FIELD(TilingFactor, Float);
		WE_SCHEMA_END
	};

	struct CircleRendererComponent
	{
		glm::vec4 Color { 1.0f, 1.0f, 1.0f, 1.0f };
		float Thickness = 1.0f;
		float Fade = 0.005f;

		WE_SCHEMA_BODY(World, CircleRendererComponent, Component)
			WE_FIELD(Color, Vec4);
			WE_FIELD(Thickness, Float);
			WE_FIELD(Fade, Float);
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
			// P2 W3a:字段 id 显式钉住(schema-compiler 的公式值会改写这两个 id,
			// 而它们已经写进存档;显式 Id 让生成物与既有存档迁移语义一致,--check 门禁才可能为绿)。
			// Entity32:Parent 在 C++ 侧是 entt::entity(32 位句柄),schema 存 64 位整数。
			WE_FIELD(Parent, UInt64, Id(0x4849455241524331), Entity32);
			WE_FIELD(InheritTransform, Bool, Id(0x4849455241524332), Default(true));
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
		// D3:材质资产路径(相对 Game/assets,形如 materials/steel.wmat)。
		// 空 = 旧行为:用上面的 Color 直接作为基色。
		std::string MaterialPath;
		// D5:.wmodel 有节点树时,选择"第几个 mesh"(节点引用 mesh 下标);
		// 内置 primitive(cube/plane/sphere)与无 submesh 的网格忽略该字段。
		int32_t MeshIndex = 0;

		WE_SCHEMA_BODY(World, MeshRendererComponent, Component)
			// 同上:四个字段 id 已随存档/材质资产落盘(D2c/D3 期间手写),显式钉住。
			WE_FIELD(Primitive, String, Id(0x4D4553485052494D));
			WE_FIELD(Color, Vec4, Id(0x4D455348434F4C52));
			WE_FIELD(MeshPath, String, Id(0x4D45534850415448));
			WE_FIELD(MaterialPath, String, Id(0x4D4154455249414C));
			// D5:字段 id 显式钉住("MESHINDX"),默认 0;.wmodel 节点树选择 mesh 用。
			WE_FIELD(MeshIndex, Int32, Id(0x4D455348494E4458), Default(0));
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
			WE_FIELD(Camera, Object, Of(SceneCamera));
			WE_FIELD(Primary, Bool);
			WE_FIELD(FixedAspectRatio, Bool);
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
			WE_FIELD(Color, Vec3, Group("Light"));
			WE_FIELD(Intensity, Float, Group("Light"), Range(0.0f, 100.0f));
			WE_FIELD(Direction, Vec3, Group("Light"));
			WE_FIELD(CastShadow, Bool, Group("Light"));
		WE_SCHEMA_END
	};

	struct PointLightComponent
	{
		glm::vec3 Color { 1.0f, 1.0f, 1.0f };
		float Intensity = 1.0f;
		// 衰减范围(世界单位):衰减系数 = saturate(1 - d/range)^2。
		float Range = 10.0f;

		WE_SCHEMA_BODY(World, PointLightComponent, Component)
			WE_FIELD(Color, Vec3, Group("Light"));
			WE_FIELD(Intensity, Float, Group("Light"), Range(0.0f, 100.0f));
			WE_FIELD(Range, Float, Group("Light"), Range(0.0f, 1000.0f));
		WE_SCHEMA_END
	};

	// 场景级环境光:多盏时**第一盏**生效;没有该组件时用 0.25 灰默认值
	// (与 D4 之前的占位实现同观感,既有场景不会突然全黑)。
	struct AmbientLightComponent
	{
		glm::vec3 Color { 1.0f, 1.0f, 1.0f };
		float Intensity = 0.25f;

		WE_SCHEMA_BODY(World, AmbientLightComponent, Component)
			WE_FIELD(Color, Vec3, Group("Light"));
			WE_FIELD(Intensity, Float, Group("Light"), Range(0.0f, 100.0f));
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
			WE_FIELD(ScriptFilePath, String);
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
			WE_FIELD(Type, Enum, Of(BodyType));
			WE_FIELD(FixedRotation, Bool);
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
			WE_FIELD(Offset, Vec2);
			WE_FIELD(Size, Vec2);
			WE_FIELD(Density, Float);
			WE_FIELD(Friction, Float);
			WE_FIELD(Restitution, Float);
			WE_FIELD(ShowCollider, Bool);
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
			WE_FIELD(Offset, Vec2);
			WE_FIELD(Radius, Float);
			WE_FIELD(Density, Float);
			WE_FIELD(Friction, Float);
			WE_FIELD(Restitution, Float);
			WE_FIELD(ShowCollider, Bool);
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
			WE_FIELD(Type, Enum, Id(0x5242334454595045), Of(MotionType));
			WE_FIELD(Mass, Float, Id(0x524233444D415353), Range(0.0f, 100000.0f));
			WE_FIELD(LinearDamping, Float, Id(0x524233444C4E4450), Range(0.0f, 100.0f));
			WE_FIELD(AngularDamping, Float, Id(0x52423344414E4744), Range(0.0f, 100.0f));
			WE_FIELD(Friction, Float, Id(0x5242334446524943), Range(0.0f, 1.0f));
			WE_FIELD(Restitution, Float, Id(0x5242334452455354), Range(0.0f, 1.0f));
			WE_FIELD(UseGravity, Bool, Id(0x5242334447525654));
		WE_SCHEMA_END
	};

	// 盒体碰撞:HalfExtents 是**局部半尺寸**,随 TransformComponent.Scale 缩放;Offset 是局部偏移(不缩放)。
	struct BoxCollider3DComponent
	{
		glm::vec3 HalfExtents { 0.5f, 0.5f, 0.5f };
		glm::vec3 Offset { 0.0f, 0.0f, 0.0f };

		WE_SCHEMA_BODY(World, BoxCollider3DComponent, Component)
			WE_FIELD(HalfExtents, Vec3, Id(0x42334448414C4658));
			WE_FIELD(Offset, Vec3, Id(0x4233444F46465354));
		WE_SCHEMA_END
	};

	struct SphereCollider3DComponent
	{
		float Radius = 0.5f;
		glm::vec3 Offset { 0.0f, 0.0f, 0.0f };

		WE_SCHEMA_BODY(World, SphereCollider3DComponent, Component)
			WE_FIELD(Radius, Float, Id(0x5333445241444955), Range(0.0f, 100000.0f));
			WE_FIELD(Offset, Vec3, Id(0x5333444F46465354));
		WE_SCHEMA_END
	};

	// 胶囊:轴沿实体的局部 Y 轴(与 Jolt CapsuleShape 的约定一致),HalfHeight 是圆柱段半高(不含两端半球)。
	struct CapsuleCollider3DComponent
	{
		float Radius = 0.5f;
		float HalfHeight = 0.5f;
		glm::vec3 Offset { 0.0f, 0.0f, 0.0f };

		WE_SCHEMA_BODY(World, CapsuleCollider3DComponent, Component)
			WE_FIELD(Radius, Float, Id(0x4333445241444955), Range(0.0f, 100000.0f));
			WE_FIELD(HalfHeight, Float, Id(0x43334448414C4648), Range(0.0f, 100000.0f));
			WE_FIELD(Offset, Vec3, Id(0x4333444F46465354));
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
			WE_FIELD(Mode, Enum, Id(0x4D33444D4F444530), Of(ColliderMode));
			WE_FIELD(MeshPath, String, Id(0x4D33445041544830));
		WE_SCHEMA_END
	};

	// 组件配置克隆特化(剔除运行态)。
	NativeScriptComponent CloneComponentConfiguration(const NativeScriptComponent& source);
	LuaScriptComponent CloneComponentConfiguration(const LuaScriptComponent& source);
	RigidBody2DComponent CloneComponentConfiguration(const RigidBody2DComponent& source);
}
