#pragma once

#include "World/Core/StringPool.h"
#include "World/Core/UUID.h"
#include "World/Core/Memory/Memory.h"
#include "World/Scene/Entity.h"
#include "World/Scene/SceneCamera.h"
#include "World/Scene/Systems/TransformSystem.h"
#include "World/Renderer/Camera.h"
#include "World/Schema/Schema.h"
#include "World/Schema/BuiltinAssetOps.h"

#include <any>
#include <box2d/id.h>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <unordered_map>

namespace World
{
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
		// 实体名 = **驻留名字标识**(NameId,4B POD),不是 std::string:
		// 组件因此平凡可拷贝(复制实体/Prefab 实例化是 memcpy),也不再有堆分配。
		// 名字不是身份(允许重名);人类可读文本经 StringPool::NameOf 取回。
		NameId Tag;

		TagComponent() = default;
		// 便捷构造:边界字符串 → 驻留名字(调用方显式给字符串时才付驻留成本)。
		explicit TagComponent(const std::string& tag) : Tag(StringPool::Get().InternName(tag)) {}

		WE_SCHEMA_BODY(World, TagComponent, Component)
			WE_SCHEMA_META(Category("Scene"),
				Core(),
				Doc("Human-readable entity label used by the hierarchy, the AI command channel and log messages; not an identifier."))
			WE_FIELD(Tag, Name);
		WE_SCHEMA_END
	};

	struct TransformComponent
	{
		glm::vec3 Location { 0.0f, 0.0f, 0.0f };
		uint32_t Flags { TransformFlags::DirtyLocal | TransformFlags::DirtyWorld };
		glm::quat Rotation { 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale { 1.0f, 1.0f, 1.0f };
		float _Padding { 0.0f };

		TransformComponent() = default;
		TransformComponent(const glm::mat4& transform) { TransformSystem::SetTransform(*this, transform); }
		TransformComponent(const glm::vec3& location, const glm::vec3& rotationEuler = glm::vec3 { 0.0f }, const glm::vec3& scale = glm::vec3 { 1.0f })
		{
			TransformSystem::SetTransform(*this, location, rotationEuler, scale);
		}
		TransformComponent(const glm::vec3& location, const glm::quat& rotation, const glm::vec3& scale = glm::vec3 { 1.0f })
		{
			TransformSystem::SetTransform(*this, location, rotation, scale);
		}

		// 变换解算与设置统一委托给 TransformSystem (轻量置脏)
		void SetLocation(const glm::vec3& location) { TransformSystem::SetLocation(*this, location); }
		void SetRotation(const glm::vec3& rotationEuler) { TransformSystem::SetRotation(*this, rotationEuler); }
		void SetRotation(const glm::quat& rotation) { TransformSystem::SetRotation(*this, rotation); }
		void SetRotationQuat(const glm::quat& rotationQuat) { TransformSystem::SetRotationQuat(*this, rotationQuat); }
		void SetScale(const glm::vec3& scale) { TransformSystem::SetScale(*this, scale); }
		void SetTransform(const glm::vec3& location, const glm::vec3& rotationEuler, const glm::vec3& scale)
		{
			TransformSystem::SetTransform(*this, location, rotationEuler, scale);
		}
		void SetTransform(const glm::vec3& location, const glm::quat& rotation, const glm::vec3& scale)
		{
			TransformSystem::SetTransform(*this, location, rotation, scale);
		}
		void SetTransform(const glm::mat4& transform) { TransformSystem::SetTransform(*this, transform); }
		void RecalculateTransform() { TransformSystem::Recalculate(*this); }

		// 局部变换矩阵与欧拉角便捷访问
		glm::mat4 GetLocalMatrix() const { return TransformSystem::Compose(Location, Rotation, Scale); }
		glm::mat4 GetTransform() const { return GetLocalMatrix(); }
		operator glm::mat4() const { return GetLocalMatrix(); }
		glm::vec3 GetEulerAngles() const { return TransformSystem::ToEulerRadians(Rotation); }

		// 脏标记管理
		bool IsDirty() const { return (Flags & (TransformFlags::DirtyLocal | TransformFlags::DirtyWorld)) != 0; }
		void SetDirty() { Flags |= (TransformFlags::DirtyLocal | TransformFlags::DirtyWorld); }
		void ClearDirty() { Flags &= ~(TransformFlags::DirtyLocal | TransformFlags::DirtyWorld); }

		WE_SCHEMA_BODY(World, TransformComponent, Component)
			WE_SCHEMA_META(Category("Scene"),
				Core(),
				Doc("Local translation/rotation/scale of the entity; 48-byte compact POD."))
			WE_FIELD(Location, Vec3, Group("Transform"),
				Doc("Local position in the parent's space (world units)."));
			WE_FIELD(Rotation, Quat, Group("Transform"),
				Doc("Local rotation stored as quaternion."));
			WE_FIELD(Scale, Vec3, Group("Transform"),
				Doc("Local scale per axis; 1,1,1 = unscaled, negative values mirror."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(TransformComponent) == 48, "TransformComponent must be 48 bytes");
	static_assert(std::is_trivially_copyable_v<TransformComponent>, "TransformComponent must be trivially copyable");

	// Pure ECS: 速度组件 (用于 MovementSystem 驱动实体线速度与角速度位移)
	struct VelocityComponent
	{
		glm::vec3 Linear { 0.0f, 0.0f, 0.0f };
		glm::vec3 Angular { 0.0f, 0.0f, 0.0f };

		VelocityComponent() = default;
		VelocityComponent(const glm::vec3& linear, const glm::vec3& angular = glm::vec3 { 0.0f })
			: Linear(linear), Angular(angular) {}

		WE_SCHEMA_BODY(World, VelocityComponent, Component)
			WE_SCHEMA_META(Category("Movement"),
				Doc("Linear and angular velocity of the entity for ECS movement systems."))
			WE_FIELD(Linear, Vec3, Group("Velocity"),
				Doc("Linear velocity in world units per second."));
			WE_FIELD(Angular, Vec3, Group("Velocity"),
				Doc("Angular velocity in radians per second."));
		WE_SCHEMA_END
	};

	// 2D 精灵:纹理引用也是**驻留 PathId**(不是 Ref<Texture2D>)——运行态对象不进组件,
	// 解析由 TextureLibrary(键 = PathId,进程内驻留)在渲染时 O(1) 完成。
	struct SpriteComponent
	{
		glm::vec4 Color { 1.0f, 1.0f, 1.0f, 1.0f };
		AssetRef Texture;
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
		bool InheritTransform = true;

		WE_SCHEMA_BODY(World, HierarchyComponent, Component)
			WE_SCHEMA_META(Category("Scene"),
				Doc("Parent link plus whether the parent transform is inherited. The runtime child list lives in the non-schema HierarchyChildrenComponent."))
			// P2 W3a:字段 id 显式钉住(schema-compiler 的公式值会改写这两个 id,
			// 而它们已经写进存档;显式 Id 让生成物与既有存档迁移语义一致,--check 门禁才可能为绿)。
			// Entity32:Parent 在 C++ 侧是 entt::entity(32 位句柄),schema 存 64 位整数。
			WE_FIELD(Parent, UInt64, Id(0x4849455241524331), Entity32,
				Doc("Parent entity; cleared = the entity is a root."));
			WE_FIELD(InheritTransform, Bool, Id(0x4849455241524332), Default(true),
				Doc("When on, this entity's world transform is parent world x local; off = ignore the parent transform."));
		WE_SCHEMA_END
	};

	// WP2(PECS 1.1):运行期子列表缓存。**非 schema**(不入 .wd、不进属性面板、不参与 schema 复制),
	// 读档后由 SceneSerializer 按 Parent 重建,写路径由 Hierarchy::SetParent / InsertChild 维护。
	// 拆出来是为了让 HierarchyComponent 保持平凡可拷贝(原 struct 内嵌 std::vector 会带 24B 堆指针)。
	struct HierarchyChildrenComponent
	{
		std::vector<entt::entity> Children;
	};

	struct WorldTransformComponent
	{
		glm::mat4 Matrix { 1.0f };
	};

	// P6:固定步长 → 渲染插值的内部状态。
	//
	// **非 schema**:不入 .wd、不进属性面板、不参与序列化(与 WorldTransformComponent 同口径)。
	// 只由物理系统(步进前记录)与渲染抽取(写 Model 时读取)使用:
	//   * PreviousLocalMatrix = 上一固定步开始时该实体的**局部**变换;
	//   * Valid = 是否已有可用的上一帧样本(新建实体第一帧为 false,不插值)。
	struct PhysicsInterpolationState
	{
		// 权威数据是局部变换(Location/Rotation/Scale)⇒ 插值也必须作用在局部分量上,
		// 再由渲染侧做一次层级合成。若插值世界矩阵,父子的相对关系会在插值后失真
		// (父被插值、子用权威世界矩阵 ⇒ 子相对父每固定步跳一次)。
		glm::mat4 PreviousLocalMatrix { 1.0f };
		bool Valid = false;
	};

	// Mesh 指向导入的模型资产(D5);Mesh 为空时用下面的内置图元 Primitive。
	// 2026-10-04:组件内**不再有任何字符串/堆持有对象** —— 资产定位符 = 驻留 PathId(4B),
	// 内置图元 = 固定三值枚举,因此组件平凡可拷贝(复制/Prefab 实例化 = memcpy),
	// 热路径也不再做每帧字符串归一化(见 Core/StringPool.h 与 contract.asset-identity-and-strings)。
	struct MeshRendererComponent
	{
		// 内置网格形状:固定三值集合 ⇒ 枚举(标识符是值,不是缓冲区)。Mesh 非空时忽略。
		enum class PrimitiveShape
		{
			Cube = 0, Sphere, Plane
		};
		WE_ENUM_SCHEMA(World, PrimitiveShape, Int32)
			WE_ENUM_VALUE(Cube);
			WE_ENUM_VALUE(Sphere);
			WE_ENUM_VALUE(Plane);
		WE_ENUM_END

		PrimitiveShape Primitive = PrimitiveShape::Cube;
		glm::vec4 Color { 1.0f, 1.0f, 1.0f, 1.0f };
		AssetRef Mesh;
		// D3:材质资产路径(相对项目内容根,形如 materials/steel.wmat)。
		// 空 = 旧行为:用上面的 Color 直接作为基色。
		AssetRef Material;
		// D5:.wmodel 有节点树时,选择"第几个 mesh"(节点引用 mesh 下标);
		// 内置 primitive(cube/plane/sphere)与无 submesh 的网格忽略该字段。
		int32_t MeshIndex = 0;

		WE_SCHEMA_BODY(World, MeshRendererComponent, Component)
			WE_SCHEMA_META(Category("Rendering/Mesh"),
				Doc("Draws a built-in primitive or an imported mesh; MeshIndex selects the mesh inside the model and an empty Material shades with Color."))
			// 同上:四个字段 id 已随存档/材质资产落盘(D2c/D3 期间手写),显式钉住。
			WE_FIELD(Primitive, Enum, Id(0x4D4553485052494D), Of(PrimitiveShape),
				Doc("Built-in primitive used when Mesh is empty."));
			WE_FIELD(Color, Vec4, Id(0x4D455348434F4C52), Color(),
				Doc("Base color used when Material is empty."));
			WE_FIELD(Mesh, Asset, Id(0x4D45534850415448), Of("Model"),
				Doc("Imported model asset (.wmodel, path relative to the project content root); empty = use Primitive. glTF/GLB are import sources only: import them first and reference the produced .wmodel."));
			WE_FIELD(Material, Asset, Id(0x4D4154455249414C), Of("Material"),
				Doc("Material asset (.wmat); overrides Color and the model's own material slots when set."));
			// D5:字段 id 显式钉住("MESHINDX"),默认 0;.wmodel 节点树选择 mesh 用。
			WE_FIELD(MeshIndex, Int32, Id(0x4D455348494E4458), Default(0));
		WE_SCHEMA_END
	};

	// 内置图元的**边界字符串**(属性面板 / AI 通道 / 暂存文档文本编辑共用一份,避免各自硬编码)。
	// 内存里恒为枚举;字符串只在 IO/UI 边界出现(见 contract.asset-identity-and-strings)。
	inline const char* PrimitiveShapeName(MeshRendererComponent::PrimitiveShape shape)
	{
		switch (shape)
		{
			case MeshRendererComponent::PrimitiveShape::Cube:   return "Cube";
			case MeshRendererComponent::PrimitiveShape::Sphere: return "Sphere";
			case MeshRendererComponent::PrimitiveShape::Plane:  return "Plane";
		}
		return "Cube";
	}

	// 接受枚举名("Cube"/"Sphere"/"Plane",大小写不敏感)或十进制下标;失败返回 false 且不改 out。
	inline bool ParsePrimitiveShape(const std::string& text, MeshRendererComponent::PrimitiveShape* out)
	{
		if (!out) return false;
		std::string lower;
		lower.reserve(text.size());
		for (char c : text)
			lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		if (lower == "cube" || lower == "0")   { *out = MeshRendererComponent::PrimitiveShape::Cube;   return true; }
		if (lower == "sphere" || lower == "1") { *out = MeshRendererComponent::PrimitiveShape::Sphere; return true; }
		if (lower == "plane" || lower == "2")  { *out = MeshRendererComponent::PrimitiveShape::Plane;  return true; }
		return false;
	}

	// P1b D5c-4a:蒙皮网格渲染组件(骨骼动画)。
	// 路径语义与 MeshRendererComponent 一致:.wmodel 是**相对内容根**的逻辑路径
	// (如 models/rock.wmodel),MeshIndex 选择模型里的第几个 mesh(该 mesh 的 SkinIndex
	// 决定用哪套骨架);Material 空 = 走模型的材质槽/Color 回退。
	// AnimationClip 空 = 第 0 条 clip;Time 是动画系统每帧写入的运行态(允许进 schema,
	// Play/Simulate 下由 AnimationSystem 写,属性面板只读)。
	// 字段 id 全部显式钉住("SK…" ASCII):一旦写进 .wd 存档就不能再改,否则旧场景迁移语义漂移。
	struct SkinnedMeshRendererComponent
	{
		AssetRef Mesh;
		int32_t MeshIndex = 0;
		AssetRef Material;
		// clip 名 = 驻留名字(在模型的若干条 clip 里查名字,天然是有界集合)。
		NameId AnimationClip;
		bool Playing = true;
		float Speed = 1.0f;
		bool Loop = true;
		float Time = 0.0f;

		WE_SCHEMA_BODY(World, SkinnedMeshRendererComponent, Component)
			WE_SCHEMA_META(Category("Rendering/Mesh"),
				Doc("Skeleton-driven mesh: an empty AnimationClip plays the first clip and Time is written by the animation system (read-only under Play)."))
			WE_FIELD(Mesh, Asset, Id(0x534B4D4553485041), Of("Model"),
				Doc("Skinned model asset (.wmodel); required — this component draws nothing without it."));
			WE_FIELD(MeshIndex, Int32, Id(0x534B4D4553484958), Default(0));
			WE_FIELD(Material, Asset, Id(0x534B4D4154505448), Of("Material"),
				Doc("Material asset (.wmat); empty = the model's own material slots."));
			WE_FIELD(AnimationClip, Name, Id(0x534B414E494D434C),
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
		CameraSettings Camera;
		bool Primary = true;
		bool FixedAspectRatio = false;

		WE_SCHEMA_BODY(World, CameraComponent, Component)
			WE_SCHEMA_META(Category("Scene"),
				Doc("Scene camera settings plus Primary (the camera Play and the runtime render through); FixedAspectRatio keeps the projection from following the viewport size."))
			WE_FIELD(Camera, Object, Of(CameraSettings));
			WE_FIELD(Primary, Bool,
				Doc("The scene renders through the first primary camera in Play and in the runtime."));
			WE_FIELD(FixedAspectRatio, Bool,
				Doc("Keep the projection aspect from the editor instead of following the viewport/window size."));
		WE_SCHEMA_END
	};

	// PECS(相机组件数据导向化):投影矩阵这类派生量不进组件,改由 CameraSystem 按
	// (CameraSettings 参数, 有效宽高比, 视口尺寸)指纹脏标记缓存到本组件。
	//
	// **非 schema**:不入 .wd、不进属性面板、不参与序列化(与 WorldTransformComponent 同口径)。
	// 由 Scene 在结构提交点预建(OnRuntimeStart / 组件新增),运行期只更新字段值。
	struct CameraViewComponent
	{
		Camera View { glm::mat4(1.0f) };   // 帧数据:只含投影矩阵
		CameraSettings LastParams {};        // 指纹:上次算投影时用的(已钳制)参数
		float LastAspectRatio = 1.0f;
		uint32_t LastViewportWidth = 0;
		uint32_t LastViewportHeight = 0;
		bool Valid = false;
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
		// WP3(PECS 1.1):running-state b2BodyId lives in the Scene-internal table
		// (same shape as 3D's Physics3DWorld::Impl::m_Bodies). The component keeps only
		// schema fields, so copies/prefab instances can never carry another entity's handle.
		bool FixedRotation = false;
		// P7:连续碰撞检测(Box2D b2BodyDef.isBullet)—— 高速小物体不会隧穿。
		bool Ccd = false;
		// P5 碰撞过滤(Box2D b2Filter 口径):Layer = 本体的类别位,Mask = 允许与本体碰撞的类别位。
		// 两个形变体必须**互相**通过 (LayerA & MaskB) && (LayerB & MaskA) 才会产生接触/事件。
		std::uint32_t Layer = 1u;
		std::uint32_t Mask = 0xFFFFFFFFu;

		WE_SCHEMA_BODY(World, RigidBody2DComponent, Component)
			WE_SCHEMA_META(Category("Physics/2D"),
				Doc("Box2D body: BodyType selects Static/Dynamic/Kinematic and FixedRotation locks the angular degree of freedom."))
			WE_FIELD(Type, Enum, Of(BodyType),
				Doc("Static never moves, Dynamic is driven by forces/gravity, Kinematic moves only through code."));
			WE_FIELD(FixedRotation, Bool,
				Doc("Lock the angular degree of freedom so collisions cannot rotate the body."));
			WE_FIELD(Layer, UInt32,
				Doc("Collision category bits of this body. Two bodies interact only when (LayerA and MaskB) and (LayerB and MaskA) are both non-zero."));
			WE_FIELD(Mask, UInt32,
				Doc("Collision category bits this body accepts. Two bodies interact only when (LayerA and MaskB) and (LayerB and MaskA) are both non-zero."));
			WE_FIELD(Ccd, Bool,
				Doc("Continuous collision detection: sweep this fast body so it cannot tunnel through thin geometry."));
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
		// P5:传感器 = 只发 TriggerEvent、不产生碰撞响应(Box2D 逐 shape 的 isSensor)。
		bool IsSensor = false;

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
			WE_FIELD(IsSensor, Bool,
				Doc("Sensor: report overlaps as trigger events but never block or bounce against other bodies."));
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
		// P5:传感器 = 只发 TriggerEvent、不产生碰撞响应(Box2D 逐 shape 的 isSensor)。
		bool IsSensor = false;

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
			WE_FIELD(IsSensor, Bool,
				Doc("Sensor: report overlaps as trigger events but never block or bounce against other bodies."));
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
		// P7:连续碰撞检测(Jolt EMotionQuality::LinearCast)—— 高速小物体不会隧穿。
		bool Ccd = false;
		// P5 碰撞过滤(Jolt ObjectLayer 口径):Layer = 本体的类别位,Mask = 允许与本体碰撞的类别位。
		// 两个刚体必须**互相**通过 (LayerA & MaskB) && (LayerB & MaskA) 才会产生接触/事件。
		std::uint32_t Layer = 1u;
		std::uint32_t Mask = 0xFFFFFFFFu;
		// P5 传感器:Jolt 的传感器是**刚体级**(整刚体只发 TriggerEvent、不产生碰撞响应)。
		// 与 2D 的"逐 shape"语义不同 —— 见 docs/dev/scripting-architecture.md。
		bool IsSensor = false;

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
				Doc("Apply the scene gravity to this body (see Project Settings ▶ Physics)."));
			WE_FIELD(Layer, UInt32,
				Doc("Collision category bits of this body. Two bodies interact only when (LayerA and MaskB) and (LayerB and MaskA) are both non-zero."));
			WE_FIELD(Mask, UInt32,
				Doc("Collision category bits this body accepts. Two bodies interact only when (LayerA and MaskB) and (LayerB and MaskA) are both non-zero."));
			WE_FIELD(IsSensor, Bool,
				Doc("Sensor: the whole body reports overlaps as trigger events but never blocks or bounces (Jolt sensors are body-wide)."));
			WE_FIELD(Ccd, Bool,
				Doc("Continuous collision detection: sweep this fast body so it cannot tunnel through thin geometry."));
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

	// P7:关节/约束(两个刚体之间)。
	//
	// 归属:关节写在**主实体**上,`Connected` 指向另一个实体;两侧必须是同一后端
	// (主实体挂 RigidBody2DComponent ⇒ Box2D 关节;挂 RigidBody3DComponent ⇒ Jolt 约束)。
	// 两个实体各自挂一份、互相引用同一个 Connected 会重复建关节 —— 校验期拒绝。
	//
	// 局部连接点:`AnchorSelf` 在主实体局部空间,`AnchorOther` 在对方实体局部空间
	// (2D 用 x/y,z 忽略;3D 用 xyz)。`Axis` 是 Hinge 的旋转轴(主实体局部空间)。
	struct JointComponent
	{
		enum class JointKind
		{
			Fixed = 0, Distance, Hinge
		};
		WE_ENUM_SCHEMA(World, JointKind, Int32)
			WE_ENUM_VALUE(Fixed);
			WE_ENUM_VALUE(Distance);
			WE_ENUM_VALUE(Hinge);
		WE_ENUM_END

		entt::entity Connected = entt::null;
		JointKind Type = JointKind::Fixed;
		glm::vec3 AnchorSelf { 0.0f };
		glm::vec3 AnchorOther { 0.0f };
		// Hinge 的旋转轴(主实体局部空间,无需归一化)。
		glm::vec3 Axis { 0.0f, 1.0f, 0.0f };
		// Distance:允许的距离区间;< 0 = 用启动时两锚点之间的实际距离。
		float MinDistance = -1.0f;
		float MaxDistance = -1.0f;
		// 关节连接的两体之间是否仍然允许碰撞(默认否,与 Unity / Box2D 默认一致)。
		bool EnableCollision = false;
		// WP3:the 2D b2JointId lives in the Scene-internal table; the component keeps only schema fields.

		WE_SCHEMA_BODY(World, JointComponent, Component)
			WE_SCHEMA_META(Category("Physics"),
				Doc("Constraint between this entity and another body. Fixed welds the two frames, Distance keeps them within a range, Hinge allows rotation around Axis only."))
			WE_FIELD(Connected, UInt64, Entity32,
				Doc("The other body of the joint. Both entities must use the same physics backend (2D or 3D)."));
			WE_FIELD(Type, Enum, Of(JointKind),
				Doc("Fixed = rigid weld, Distance = keep a distance range, Hinge = rotation around Axis only."));
			WE_FIELD(AnchorSelf, Vec3,
				Doc("Joint anchor in this entity's local space."));
			WE_FIELD(AnchorOther, Vec3,
				Doc("Joint anchor in the connected entity's local space."));
			WE_FIELD(Axis, Vec3,
				Doc("Hinge rotation axis in this entity's local space (ignored by Fixed and Distance)."));
			WE_FIELD(MinDistance, Float,
				Doc("Distance joints: minimum allowed separation; negative = derived from the anchors at start."));
			WE_FIELD(MaxDistance, Float,
				Doc("Distance joints: maximum allowed separation; negative = derived from the anchors at start."));
			WE_FIELD(EnableCollision, Bool,
				Doc("Allow the two connected bodies to collide with each other (off by default)."));
		WE_SCHEMA_END
	};

	// 网格碰撞:Mesh 空 = 用同实体 MeshRendererComponent.Mesh(相对内容根的 .wmodel 路径);
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
		AssetRef Mesh;

		WE_SCHEMA_BODY(World, MeshCollider3DComponent, Component)
			WE_SCHEMA_META(Category("Physics/3D"),
				Doc("Mesh-derived collision: an empty Mesh uses the entity's MeshRenderer mesh, StaticTriangles only works on Static bodies, and the editor draws no outline for it."))
			WE_FIELD(Mode, Enum, Id(0x4D33444D4F444530), Of(ColliderMode),
				Doc("ConvexHull works on every body type; StaticTriangles is only allowed on Static bodies."));
			WE_FIELD(Mesh, Asset, Id(0x4D33445041544830), Of("Model"),
				Doc("Collision model asset (.wmodel); empty = reuse the MeshRenderer mesh on the same entity. glTF/GLB sources must be imported to .wmodel first."));
		WE_SCHEMA_END
	};

}
