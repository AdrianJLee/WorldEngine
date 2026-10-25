#pragma once

#include "World/Core/StringPool.h"
#include "World/Physics/Physics2DUtils.h"
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
#include <cstddef>
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
				Doc("Persistence identity written into the entity document so entities can be matched across save and load; every entity is expected to carry one."))
			WE_FIELD(ID, Object, Of(UUID));
		WE_SCHEMA_END
	};

	struct HealthComponent
	{
		float CurrentHealth = 100.0f;                // 4B
		float MaxHealth = 100.0f;                    // 4B
		float Shield = 0.0f;                         // 4B
		float MaxShield = 0.0f;                      // 4B
		float Armor = 0.0f;                          // 4B
		float InvulnerableDuration = 0.5f;           // 4B
		float InvulnerableTimer = 0.0f;              // 4B
		bool IsDead = false;                         // 1B
		uint8_t MitigationMode = 0;                  // 1B (0: Hyperbolic, 1: Flat, 2: Percentage, 3: Custom)
		uint8_t Flags = 0;                           // 1B
		uint8_t _Padding[1] { 0 };                   // 1B

		HealthComponent() = default;

		WE_SCHEMA_BODY(World, HealthComponent, Component)
			WE_SCHEMA_META(Category("Gameplay"),
				Doc("Entity health, shields, armor damage mitigation, and invulnerability frames."))
			WE_FIELD(CurrentHealth, Float,
				Doc("Current health points."));
			WE_FIELD(MaxHealth, Float, Range(1.0f, 10000.0f),
				Doc("Maximum health points."));
			WE_FIELD(Shield, Float,
				Doc("Current protective shield points."));
			WE_FIELD(MaxShield, Float, Range(0.0f, 10000.0f),
				Doc("Maximum protective shield points."));
			WE_FIELD(Armor, Float, Range(0.0f, 1000.0f),
				Doc("Armor points for damage reduction formula."));
			WE_FIELD(InvulnerableDuration, Float, Range(0.0f, 5.0f),
				Doc("I-Frame invulnerability duration upon receiving damage."));
			WE_FIELD(IsDead, Bool,
				Doc("True if entity health reached zero."));
		WE_SCHEMA_END
	};

	enum class SmartObjectSlotState : uint8_t
	{
		Available = 0,
		Claimed = 1,
		InUse = 2,
		Cooldown = 3
	};
	WE_ENUM_SCHEMA(World, SmartObjectSlotState, UInt8)
		WE_ENUM_VALUE(Available);
		WE_ENUM_VALUE(Claimed);
		WE_ENUM_VALUE(InUse);
		WE_ENUM_VALUE(Cooldown);
	WE_ENUM_END

	enum class ParticleEmitterType : uint8_t
	{
		Point = 0,
		Line = 1,
		Box = 2,
		Sphere = 3,
		Cone = 4,
		Circle = 5
	};
	WE_ENUM_SCHEMA(World, ParticleEmitterType, UInt8)
		WE_ENUM_VALUE(Point);
		WE_ENUM_VALUE(Line);
		WE_ENUM_VALUE(Box);
		WE_ENUM_VALUE(Sphere);
		WE_ENUM_VALUE(Cone);
		WE_ENUM_VALUE(Circle);
	WE_ENUM_END

	enum class ParticleRenderMode : uint8_t
	{
		Billboard = 0,
		Quad2D = 1,
		StretchedVelocity = 2,
		HorizontalQuad = 3
	};
	WE_ENUM_SCHEMA(World, ParticleRenderMode, UInt8)
		WE_ENUM_VALUE(Billboard);
		WE_ENUM_VALUE(Quad2D);
		WE_ENUM_VALUE(StretchedVelocity);
		WE_ENUM_VALUE(HorizontalQuad);
	WE_ENUM_END

	enum class ParticleBlendMode : uint8_t
	{
		AlphaBlend = 0,
		Additive = 1,
		Multiply = 2,
		Opaque = 3
	};
	WE_ENUM_SCHEMA(World, ParticleBlendMode, UInt8)
		WE_ENUM_VALUE(AlphaBlend);
		WE_ENUM_VALUE(Additive);
		WE_ENUM_VALUE(Multiply);
		WE_ENUM_VALUE(Opaque);
	WE_ENUM_END



	enum class VfxPlaybackState : uint8_t
	{
		Stopped = 0,
		Playing = 1,
		Paused = 2
	};
	WE_ENUM_SCHEMA(World, VfxPlaybackState, UInt8)
		WE_ENUM_VALUE(Stopped);
		WE_ENUM_VALUE(Playing);
		WE_ENUM_VALUE(Paused);
	WE_ENUM_END

	struct VfxComponent
	{
		AssetRef Effect;                                // 16B (Offset 0: R1 8B-aligned reference to .wvfx asset)
		glm::vec4 TintColor { 1.0f, 1.0f, 1.0f, 1.0f }; // 16B (Offset 16: RGBA tint color & HDR emissive boost)
		float PlaybackSpeed = 1.0f;                    // 4B (Offset 32: Simulation rate multiplier 0.0=paused, 1.0=normal)
		float Intensity = 1.0f;                        // 4B (Offset 36: Master emission rate & density multiplier)
		float WarmupTime = 0.0f;                       // 4B (Offset 40: Pre-simulation time on spawn in seconds)
		float CullDistance = 60.0f;                    // 4B (Offset 44: Camera culling distance in meters)
		uint32_t Seed = 0;                             // 4B (Offset 48: Deterministic PRNG seed, 0 = random)
		VfxPlaybackState PlaybackState = VfxPlaybackState::Playing; // 1B (Offset 52: Active playback state)
		bool PlayOnAwake = true;                       // 1B (Offset 53: Auto start playing on scene start)
		bool Looping = true;                           // 1B (Offset 54: Loop playback sequence)
		bool PauseWhenOffscreen = true;                // 1B (Offset 55: Freeze simulation when culled by camera)
		uint8_t _Padding[8] { 0 };                     // 8B (Offset 56..63: Explicit padding to 64B)

		VfxComponent() = default;

		WE_SCHEMA_BODY(World, VfxComponent, Component)
			WE_SCHEMA_META(Category("Effects"),
				Doc("Next-generation unified VFX component for multi-emitter assets, ribbons, meshes and dynamic lights."))
			WE_FIELD(Effect, Asset, Of("VFX"),
				Doc("Bound .wvfx VFX asset defining emitters, ribbons, meshes and dynamic lights."));
			WE_FIELD(TintColor, Vec4, Color(),
				Doc("Global tint color and HDR emissive boost applied to all emitters."));
			WE_FIELD(PlaybackSpeed, Float, Range(0.0f, 10.0f),
				Doc("Simulation playback speed multiplier (0.0 = paused, 1.0 = normal)."));
			WE_FIELD(Intensity, Float, Range(0.0f, 10.0f),
				Doc("Master emission rate and particle density multiplier."));
			WE_FIELD(WarmupTime, Float, Range(0.0f, 30.0f),
				Doc("Pre-simulation time in seconds on spawn so effect begins fully formed."));
			WE_FIELD(CullDistance, Float, Range(1.0f, 1000.0f),
				Doc("Camera distance in meters beyond which particle simulation is paused."));
			WE_FIELD(Seed, UInt32,
				Doc("Deterministic seed for reproducible simulation (0 uses randomized seed)."));
			WE_FIELD(PlaybackState, Enum, Of(VfxPlaybackState),
				Doc("Current playback state (Playing, Paused, Stopped)."));
			WE_FIELD(PlayOnAwake, Bool,
				Doc("Automatically begin playback when the scene starts."));
			WE_FIELD(Looping, Bool,
				Doc("Loop the VFX sequence continuously."));
			WE_FIELD(PauseWhenOffscreen, Bool,
				Doc("Pause simulation when bounding volume is outside camera view frustum."));
		WE_SCHEMA_END
	};

	struct SmartObjectComponent
	{
		glm::vec3 SlotOffset { 0.0f, 0.0f, 0.0f };   // 12B
		float FacingAngleDeg = 0.0f;                 // 4B
		float CooldownDuration = 3.0f;               // 4B
		float CooldownTimer = 0.0f;                  // 4B
		float InteractionDistance = 2.0f;            // 4B
		uint32_t ActivityTag = 0;                    // 4B
		uint32_t ClaimedByEntity = 0;                // 4B
		SmartObjectSlotState SlotState = SmartObjectSlotState::Available; // 1B
		bool Enabled = true;                         // 1B
		uint8_t _Padding[6] { 0 };                   // 6B

		SmartObjectComponent() = default;

		WE_SCHEMA_BODY(World, SmartObjectComponent, Component)
			WE_SCHEMA_META(Category("AI"),
				Doc("Smart Object interaction slot: position alignment, claim/use reservation state machine, and cooldown."))
			WE_FIELD(SlotOffset, Vec3,
				Doc("Interaction slot offset relative to object transform."));
			WE_FIELD(FacingAngleDeg, Float, Range(0.0f, 360.0f),
				Doc("Expected facing direction angle in degrees upon using slot."));
			WE_FIELD(CooldownDuration, Float, Range(0.0f, 60.0f),
				Doc("Cooldown duration after use in seconds."));
			WE_FIELD(InteractionDistance, Float, Range(0.1f, 10.0f),
				Doc("Maximum radius allowing interaction."));
			WE_FIELD(SlotState, Enum, Of(SmartObjectSlotState),
				Doc("Current reservation state (Available, Claimed, InUse, Cooldown)."));
			WE_FIELD(Enabled, Bool,
				Doc("Whether this smart object is actively available for interaction."));
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
			WE_SCHEMA_META(Category("Gameplay"),
				Doc("Linear and angular velocity of the entity for ECS movement systems."))
			WE_FIELD(Linear, Vec3, Group("Velocity"),
				Doc("Linear velocity in world units per second."));
			WE_FIELD(Angular, Vec3, Group("Velocity"),
				Doc("Angular velocity in radians per second."));
		WE_SCHEMA_END
	};

	// 2D 精灵:纹理引用也是**驻留 PathId**(不是 Ref<Texture2D>)——运行态对象不进组件,
	// 解析由 TextureLibrary(键 = PathId,进程内驻留)在渲染时 O(1) 完成。
	// 支持单图、图集切片子精灵(.watlas)、自定义锚点 Pivot、X/Y 翻转、排序图层与 Y-Sort 深度。
	struct SpriteComponent
	{
		glm::vec4 Color { 1.0f, 1.0f, 1.0f, 1.0f };
		AssetRef Texture;
		glm::vec2 Pivot { 0.5f, 0.5f };
		NameId SubTextureName;
		float TilingFactor = 1.0f;
		int32_t SortingLayer = 0;
		int32_t OrderInLayer = 0;
		bool FlipX = false;
		bool FlipY = false;
		bool EnableYSort = false;
		uint8_t DrawMode = 0; // 0: Simple, 1: Sliced, 2: Tiled
		NameId NormalMap;

		SpriteComponent() = default;
		SpriteComponent(const glm::vec4& color) : Color(color) {}

		WE_SCHEMA_BODY(World, SpriteComponent, Component)
			WE_SCHEMA_META(Category("Rendering/2D"),
				Doc("2D sprite quad with support for textures, texture atlas sub-sprites, custom pivot, flipping, sorting layers, and 9-slice."))
			WE_FIELD(Color, Vec4, Color(),
				Doc("Tint multiplied into the sprite (alpha < 1 blends with what is behind)."));
			WE_FIELD(Texture, Asset, Of("Texture2D"));
			WE_FIELD(Pivot, Vec2,
				Doc("Normalized pivot point; (0.5, 0.5) = center, (0.5, 0.0) = foot center."));
			WE_FIELD(SubTextureName, Name,
				Doc("Name of the sub-sprite within the referenced TextureAtlas."));
			WE_FIELD(TilingFactor, Float,
				Doc("Repeats the texture UVs; 1 = once across the quad."));
			WE_FIELD(SortingLayer, Int32, Default(0),
				Doc("Render layer priority (Default = 0, Background = -10, Foreground = 10)."));
			WE_FIELD(OrderInLayer, Int32, Default(0),
				Doc("Order within the sorting layer (higher numbers draw on top)."));
			WE_FIELD(FlipX, Bool, Default(false),
				Doc("Horizontally flip sprite graphics."));
			WE_FIELD(FlipY, Bool, Default(false),
				Doc("Vertically flip sprite graphics."));
			WE_FIELD(EnableYSort, Bool, Default(false),
				Doc("Sort depth dynamically based on foot Y position for Top-Down games."));
			WE_FIELD(DrawMode, UInt8, Default(0),
				Doc("Draw mode: 0 = Simple, 1 = Sliced, 2 = Tiled."));
			WE_FIELD(NormalMap, Name,
				Doc("Optional normal map texture path for 2D lighting surface relief."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(SpriteComponent) == 64, "SpriteComponent must be exactly 64 bytes");
	static_assert(std::is_trivially_copyable_v<SpriteComponent>, "SpriteComponent must be trivially copyable");

	struct SpriteAnimatorComponent
	{
		AssetRef Animation;
		float Speed = 1.0f;
		float CurrentTime = 0.0f;
		uint32_t CurrentFrame = 0;
		uint8_t LoopMode = 0;
		bool Playing = true;
		bool Finished = false;
		uint8_t Reserved1 = 0;

		SpriteAnimatorComponent() = default;

		WE_SCHEMA_BODY(World, SpriteAnimatorComponent, Component)
			WE_SCHEMA_META(Category("Rendering/2D"),
				Doc("2D flipbook sprite animator that drives SpriteComponent sub-texture frames over time."))
			WE_FIELD(Animation, Asset, Of("SpriteAnimation"));
			WE_FIELD(Speed, Float, Default(1.0f),
				Doc("Playback speed multiplier."));
			WE_FIELD(CurrentTime, Float, Default(0.0f),
				Doc("Current animation elapsed time in seconds."));
			WE_FIELD(CurrentFrame, UInt32, Default(0),
				Doc("Current evaluated frame index."));
			WE_FIELD(LoopMode, UInt8, Default(0),
				Doc("0 = Loop, 1 = Once, 2 = PingPong."));
			WE_FIELD(Playing, Bool, Default(true),
				Doc("Whether the animation is currently playing."));
			WE_FIELD(Finished, Bool, Default(false),
				Doc("Whether a non-looping animation has reached the end."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(SpriteAnimatorComponent) == 32, "SpriteAnimatorComponent must be exactly 32 bytes");
	static_assert(std::is_trivially_copyable_v<SpriteAnimatorComponent>, "SpriteAnimatorComponent must be trivially copyable");

	struct TilemapComponent
	{
		glm::vec4 TintColor { 1.0f, 1.0f, 1.0f, 1.0f };
		AssetRef Tilemap;
		glm::vec2 CellSize { 1.0f, 1.0f };
		int32_t SortingLayer = -5;
		int32_t OrderInLayer = 0;
		bool GenerateCollision = true;
		bool ShowGrid = false;
		uint8_t GridType = 0;
		uint8_t ColliderType = 0; // 0 = MergedBoxes, 1 = CompositeChain, 2 = None
		uint32_t Reserved2 = 0;

		TilemapComponent() = default;

		WE_SCHEMA_BODY(World, TilemapComponent, Component)
			WE_SCHEMA_META(Category("Rendering/2D"),
				Doc("2D grid tilemap renderer and merged Box2D static collider generator."))
			WE_FIELD(TintColor, Vec4, Color(),
				Doc("Tint multiplied into all painted tiles."));
			WE_FIELD(Tilemap, Asset, Of("Tilemap"),
				Doc("Tilemap asset (.wtilemap) containing grid cells, atlas binding, and solid mask."));
			WE_FIELD(CellSize, Vec2,
				Doc("World-space size of a single grid cell in meters."));
			WE_FIELD(SortingLayer, Int32, Default(-5),
				Doc("Render layer priority (Background = -10, Tilemap = -5, Default = 0)."));
			WE_FIELD(OrderInLayer, Int32, Default(0),
				Doc("Order within the sorting layer."));
			WE_FIELD(GenerateCollision, Bool, Default(true),
				Doc("Automatically merge contiguous solid tiles into Box2D static box colliders."));
			WE_FIELD(ShowGrid, Bool, Default(false),
				Doc("Draw tile grid overlay in the editor viewport."));
			WE_FIELD(GridType, UInt8, Default(0),
				Doc("0 = Orthogonal, 1 = Isometric, 2 = HexagonalPointy, 3 = HexagonalFlat."));
			WE_FIELD(ColliderType, UInt8, Default(0),
				Doc("Collision mode: 0 = MergedBoxes (greedy rects), 1 = CompositeChain (smooth edge chain), 2 = None."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(TilemapComponent) == 56, "TilemapComponent must be exactly 56 bytes");
	static_assert(std::is_trivially_copyable_v<TilemapComponent>, "TilemapComponent must be trivially copyable");

	struct CircleRendererComponent
	{
		glm::vec4 Color { 1.0f, 1.0f, 1.0f, 1.0f };
		float Thickness = 1.0f;
		float Fade = 0.005f;

		WE_SCHEMA_BODY(World, CircleRendererComponent, Component)
			WE_SCHEMA_META(Category("Rendering/2D"),
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

	// WP2(PECS 1.1):运行期子列表缓存。**非 schema**(不入 .entity、不进属性面板、不参与 schema 复制),
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
	// **非 schema**:不入 .entity、不进属性面板、不参与序列化(与 WorldTransformComponent 同口径)。
	// 只由物理系统(步进前记录)与渲染抽取(写 Model 时读取)使用:
	//   * `PreviousLocation/Rotation/Scale` = 上一固定步开始时该实体的**局部 TRS**
	//     (权威分量,不是派生矩阵 —— 见下面 O2 的说明);
	//   * `Valid` = 是否已有可用的上一帧样本(新建实体第一帧为 false,不插值)。
	//
	// 为什么存 TRS 而不是局部矩阵(2026-10-05,O2,基准见标准 §6):
	//   1. **消费者要的就是 TRS**:`InterpolateRigidTransform` 必须先 normalize 矩阵的三列、
	//      `quat_cast` 回四元数、取列长当缩放,才能 slerp —— 存矩阵等于"合成了又拆开"。
	//      直接存 TRS 让这条路一次往返都不做。
	//   2. **生产者本来就有 TRS**:`TransformComponent` 的权威数据就是 Location/Rotation/Scale;
	//      存矩阵要在固定步里多算一次 Compose。改存 TRS 后写侧只做 40B 拷贝。
	//   3. **装进单条 cache line**:44B ≤ 64B(原矩阵版 68B 跨两条线)。实测基准(标准 §6.3):
	//      遍历 68B 状态 1.435 ns/元素 vs 44B 状态 0.589 ns/元素。
	//   4. **语义不变**:插值仍在局部分量上做(父子相对关系不会失真),端点与 slerp 口径与
	//      旧实现一致;少的只是 Compose→Decompose 的浮点往返,因此不会更差。
	struct PhysicsInterpolationState
	{
		glm::vec3 PreviousLocation { 0.0f };
		glm::quat PreviousRotation { 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 PreviousScale { 1.0f };
		bool Valid = false;
	};
	// 布局棘轮:必须装进单条 cache line(改宽即编译失败,见 ComponentLayoutBudget.h)。
	static_assert(sizeof(PhysicsInterpolationState) == 44,
		"PhysicsInterpolationState must stay 44B (TRS + Valid) so it fits one cache line");

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

		// §4.8 R2:两个 4B 标量前置 ⇒ Color 与 8B 对齐的 AssetRef 之间不再有 4B 内部空洞。
		PrimitiveShape Primitive = PrimitiveShape::Cube;
		// D5:.wmodel 有节点树时,选择"第几个 mesh"(节点引用 mesh 下标);
		// 内置 primitive(cube/plane/sphere)与无 submesh 的网格忽略该字段。
		int32_t MeshIndex = 0;
		glm::vec4 Color { 1.0f, 1.0f, 1.0f, 1.0f };
		AssetRef Mesh;
		// D3:材质资产路径(相对项目内容根,形如 materials/steel.wmat)。
		// 空 = 旧行为:用上面的 Color 直接作为基色。
		AssetRef Material;

		WE_SCHEMA_BODY(World, MeshRendererComponent, Component)
			WE_SCHEMA_META(Category("Rendering"),
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
	// 字段 id 全部显式钉住("SK…" ASCII):一旦写进 .entity 存档就不能再改,否则旧场景迁移语义漂移。
	struct SkinnedMeshRendererComponent
	{
		AssetRef Mesh;
		AssetRef Material;
		int32_t MeshIndex = 0;
		// clip 名 = 驻留名字(在模型的若干条 clip 里查名字,天然是有界集合)。
		NameId AnimationClip;
		float Speed = 1.0f;
		float Time = 0.0f;
		bool Playing = true;
		bool Loop = true;

		WE_SCHEMA_BODY(World, SkinnedMeshRendererComponent, Component)
			WE_SCHEMA_META(Category("Rendering"),
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
			WE_SCHEMA_META(Category("Camera"),
				Doc("Scene camera settings plus Primary (the camera Play and the runtime render through); FixedAspectRatio keeps the projection from following the viewport size."))
			WE_FIELD(Camera, Object, Of(CameraSettings));
			WE_FIELD(Primary, Bool,
				Doc("The scene renders through the first primary camera in Play and in the runtime."));
			WE_FIELD(FixedAspectRatio, Bool,
				Doc("Keep the projection aspect from the editor instead of following the viewport/window size."));
		WE_SCHEMA_END
	};

	struct PixelPerfectCameraComponent
	{
		glm::ivec2 ReferenceResolution { 320, 180 };
		uint32_t PixelsPerUnit = 16;
		uint32_t CurrentScale = 1;
		bool SnapToPixelGrid = true;
		bool UpscaleRT = false;
		uint16_t Reserved = 0;
		uint32_t Reserved2 = 0;

		PixelPerfectCameraComponent() = default;

		WE_SCHEMA_BODY(World, PixelPerfectCameraComponent, Component)
			WE_SCHEMA_META(Category("Camera"),
				Doc("Pixel-perfect 2D retro camera: calculates integer-multiple viewport scaling and snaps view translation to pixel grid to eliminate jitter and stretching."))
			WE_FIELD(ReferenceResolution, IVec2,
				Doc("Target design reference resolution in pixels (e.g. 320x180, 640x360)."));
			WE_FIELD(PixelsPerUnit, UInt32, Default(16),
				Doc("Pixels Per Unit (PPU): number of pixels in one world meter/unit."));
			WE_FIELD(CurrentScale, UInt32, Default(1),
				Doc("Evaluated integer scaling ratio for the current viewport size."));
			WE_FIELD(SnapToPixelGrid, Bool, Default(true),
				Doc("Snap camera view matrix translation to the pixel grid to eliminate sub-pixel jitter."));
			WE_FIELD(UpscaleRT, Bool, Default(false),
				Doc("Whether to render to a native reference-resolution buffer then upscale with nearest-neighbor filtering."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(PixelPerfectCameraComponent) == 24, "PixelPerfectCameraComponent must be exactly 24 bytes");
	static_assert(std::is_trivially_copyable_v<PixelPerfectCameraComponent>, "PixelPerfectCameraComponent must be trivially copyable");

	struct ParallaxLayerComponent
	{
		glm::vec2 ScrollFactor { 0.5f, 0.5f };
		glm::vec2 RepeatSize { 0.0f, 0.0f };
		glm::vec2 Offset { 0.0f, 0.0f };
		bool RepeatX = true;
		bool RepeatY = false;
		uint16_t Reserved = 0;
		uint32_t Reserved2 = 0;

		ParallaxLayerComponent() = default;

		WE_SCHEMA_BODY(World, ParallaxLayerComponent, Component)
			WE_SCHEMA_META(Category("Rendering/2D"),
				Doc("2D Parallax Background Layer: scrolls at a factor of the active camera speed and supports seamless repeating."))
			WE_FIELD(ScrollFactor, Vec2,
				Doc("Scroll speed multiplier relative to camera movement (e.g. 0.1 for distant mountains, 1.2 for foreground)."));
			WE_FIELD(RepeatSize, Vec2,
				Doc("Tile repeat bounds in world units (0 = use sprite bounds)."));
			WE_FIELD(Offset, Vec2,
				Doc("Initial static layer offset."));
			WE_FIELD(RepeatX, Bool, Default(true),
				Doc("Automatically tile horizontally in camera view."));
			WE_FIELD(RepeatY, Bool, Default(false),
				Doc("Automatically tile vertically in camera view."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(ParallaxLayerComponent) == 32, "ParallaxLayerComponent must be exactly 32 bytes");
	static_assert(std::is_trivially_copyable_v<ParallaxLayerComponent>, "ParallaxLayerComponent must be trivially copyable");

	// PECS(相机组件数据导向化):投影矩阵这类派生量不进组件,改由 CameraSystem 按
	// (CameraSettings 参数, 有效宽高比, 视口尺寸)指纹脏标记缓存到本组件。
	//
	// **非 schema**:不入 .entity、不进属性面板、不参与序列化(与 WorldTransformComponent 同口径)。
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
			WE_SCHEMA_META(Category("Lighting"),
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
			WE_SCHEMA_META(Category("Lighting"),
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

	struct SpotLight3DComponent
	{
		glm::vec3 Color { 1.0f, 1.0f, 1.0f };      // 12B (offset 0)
		float Intensity = 1.0f;                    // 4B  (offset 12)
		float Range = 20.0f;                       // 4B  (offset 16)
		float InnerConeAngle = 30.0f;              // 4B  (offset 20)
		float OuterConeAngle = 45.0f;              // 4B  (offset 24)
		uint8_t CastShadow = 0;                    // 1B  (offset 28)
		uint8_t _Padding[3] { 0 };                 // 3B  (offset 29)

		SpotLight3DComponent() = default;

		WE_SCHEMA_BODY(World, SpotLight3DComponent, Component)
			WE_SCHEMA_META(Category("Lighting"),
				Doc("3D spot light with distance range, cone angles and penumbra falloff."))
			WE_FIELD(Color, Vec3, Group("Light"), Color(),
				Doc("Linear color of the spot light."));
			WE_FIELD(Intensity, Float, Group("Light"), Range(0.0f, 100.0f),
				Doc("Brightness multiplier."));
			WE_FIELD(Range, Float, Group("Light"), Range(0.1f, 1000.0f), Default(20.0f),
				Doc("Maximum illumination range in meters."));
			WE_FIELD(InnerConeAngle, Float, Group("Light"), Range(0.0f, 89.0f), Default(30.0f),
				Doc("Inner cone half-angle in degrees where light is at full intensity."));
			WE_FIELD(OuterConeAngle, Float, Group("Light"), Range(0.1f, 90.0f), Default(45.0f),
				Doc("Outer cone half-angle in degrees where light attenuates to zero."));
			WE_FIELD(CastShadow, UInt8, Group("Light"), Default(0),
				Doc("Whether this spot light casts shadows."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(SpotLight3DComponent) == 32, "SpotLight3DComponent must be exactly 32 bytes");
	static_assert(std::is_trivially_copyable_v<SpotLight3DComponent>, "SpotLight3DComponent must be trivially copyable");
	static_assert(offsetof(SpotLight3DComponent, Color) == 0, "Color offset mismatch");
	static_assert(offsetof(SpotLight3DComponent, Intensity) == 12, "Intensity offset mismatch");
	static_assert(offsetof(SpotLight3DComponent, Range) == 16, "Range offset mismatch");
	static_assert(offsetof(SpotLight3DComponent, InnerConeAngle) == 20, "InnerConeAngle offset mismatch");
	static_assert(offsetof(SpotLight3DComponent, OuterConeAngle) == 24, "OuterConeAngle offset mismatch");
	static_assert(offsetof(SpotLight3DComponent, CastShadow) == 28, "CastShadow offset mismatch");
	struct AmbientLightComponent
	{
		glm::vec3 Color { 1.0f, 1.0f, 1.0f };
		float Intensity = 0.25f;

		WE_SCHEMA_BODY(World, AmbientLightComponent, Component)
			WE_SCHEMA_META(Category("Lighting"),
				Doc("Global ambient term without a spatial range: only the first ambient light is used and 0.25 grey applies when the scene has none."))
			WE_FIELD(Color, Vec3, Group("Light"), Color(),
				Doc("Linear ambient color added to every surface (decoded with pow 2.2)."));
			WE_FIELD(Intensity, Float, Group("Light"), Range(0.0f, 100.0f),
				Doc("Ambient strength: 0 = no ambient, 0.25 is the engine default when the scene has none."));
		WE_SCHEMA_END
	};

	struct PointLight2DComponent
	{
		glm::vec3 Color { 1.0f, 1.0f, 1.0f };
		float Intensity = 1.0f;
		float InnerRadius = 0.5f;
		float OuterRadius = 5.0f;
		float FalloffIntensity = 0.5f;
		float InnerAngle = 360.0f;
		float OuterAngle = 360.0f;
		float NormalMapZ = 0.5f;
		int32_t TargetSortingLayer = 0;
		bool CastShadows = true;
		bool AffectAllLayers = true;
		bool Enabled = true;
		uint8_t _Padding = 0;

		PointLight2DComponent() = default;

		WE_SCHEMA_BODY(World, PointLight2DComponent, Component)
			WE_SCHEMA_META(Category("Lighting"),
				Doc("2D point/spot light with inner/outer radius attenuation, angular cone falloff, shadow casting, and normal map Z height."))
			WE_FIELD(Color, Vec3, Color(),
				Doc("Linear RGB color of the 2D light."));
			WE_FIELD(Intensity, Float, Default(1.0f), Range(0.0f, 50.0f),
				Doc("Brightness multiplier applied to the 2D light color."));
			WE_FIELD(InnerRadius, Float, Default(0.5f), Range(0.0f, 500.0f),
				Doc("Inner radius in world units within which light is at full intensity."));
			WE_FIELD(OuterRadius, Float, Default(5.0f), Range(0.05f, 1000.0f),
				Doc("Outer radius in world units where light attenuates to zero."));
			WE_FIELD(FalloffIntensity, Float, Default(0.5f), Range(0.0f, 1.0f),
				Doc("Falloff curve exponent control (0 = linear, 0.5 = smooth quadratic, 1 = steep)."));
			WE_FIELD(InnerAngle, Float, Default(360.0f), Range(0.0f, 360.0f),
				Doc("Inner spot cone angle in degrees (360 = omnidirectional 2D point light)."));
			WE_FIELD(OuterAngle, Float, Default(360.0f), Range(0.0f, 360.0f),
				Doc("Outer spot cone angle in degrees."));
			WE_FIELD(NormalMapZ, Float, Default(0.5f), Range(0.05f, 10.0f),
				Doc("Virtual Z height above the 2D sprite plane used for normal map lighting."));
			WE_FIELD(TargetSortingLayer, Int32, Default(0),
				Doc("Target 2D sorting layer when AffectAllLayers is false."));
			WE_FIELD(CastShadows, Bool, Default(true),
				Doc("Whether this 2D light is occluded by ShadowCaster2D silhouettes."));
			WE_FIELD(AffectAllLayers, Bool, Default(true),
				Doc("Whether this light illuminates all 2D sorting layers."));
			WE_FIELD(Enabled, Bool, Default(true),
				Doc("Whether this 2D light is active."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(PointLight2DComponent) == 48, "PointLight2DComponent must be exactly 48 bytes");
	static_assert(std::is_trivially_copyable_v<PointLight2DComponent>, "PointLight2DComponent must be trivially copyable");

	struct GlobalLight2DComponent
	{
		glm::vec3 Color { 1.0f, 1.0f, 1.0f };
		float Intensity = 1.0f;
		glm::vec2 Direction { 0.0f, -1.0f };
		float DirectionIntensity = 0.0f;
		int32_t TargetSortingLayer = 0;
		bool AffectAllLayers = true;
		bool Enabled = true;
		uint8_t _Padding[2] { 0, 0 };

		GlobalLight2DComponent() = default;

		WE_SCHEMA_BODY(World, GlobalLight2DComponent, Component)
			WE_SCHEMA_META(Category("Lighting"),
				Doc("2D global ambient and directional light controlling overall 2D scene illumination and normal map directional relief."))
			WE_FIELD(Color, Vec3, Color(),
				Doc("Global 2D ambient light color."));
			WE_FIELD(Intensity, Float, Default(1.0f), Range(0.0f, 20.0f),
				Doc("Global 2D light intensity multiplier."));
			WE_FIELD(Direction, Vec2,
				Doc("2D directional vector for global normal map relief lighting."));
			WE_FIELD(DirectionIntensity, Float, Default(0.0f), Range(0.0f, 1.0f),
				Doc("Strength of directional normal map shading on 2D sprites (0 = uniform ambient)."));
			WE_FIELD(TargetSortingLayer, Int32, Default(0),
				Doc("Target 2D sorting layer when AffectAllLayers is false."));
			WE_FIELD(AffectAllLayers, Bool, Default(true),
				Doc("Whether global 2D light affects all sorting layers."));
			WE_FIELD(Enabled, Bool, Default(true),
				Doc("Whether this global 2D light is active."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(GlobalLight2DComponent) == 36, "GlobalLight2DComponent must be exactly 36 bytes");
	static_assert(std::is_trivially_copyable_v<GlobalLight2DComponent>, "GlobalLight2DComponent must be trivially copyable");

	struct ShadowCaster2DComponent
	{
		glm::vec2 Offset { 0.0f, 0.0f };
		glm::vec2 Size { 1.0f, 1.0f };
		NameId Points;
		float ShadowIntensity = 0.85f;
		float ExtrusionLength = 12.0f;
		uint8_t ShapeType = 0; // 0 = Box, 1 = Polygon
		bool SelfShadows = false;
		bool CastShadows = true;
		bool ShowGizmo = true;

		ShadowCaster2DComponent() = default;

		inline std::vector<glm::vec2> GetLocalPolygon() const
		{
			if (ShapeType == 1 && Points.IsValid())
			{
				auto pts = Physics2D::ParsePointList(StringPool::Get().NameOf(Points));
				if (pts.size() >= 3)
					return pts;
			}
			const float hx = std::max(Size.x * 0.5f, 0.001f);
			const float hy = std::max(Size.y * 0.5f, 0.001f);
			return {
				{ -hx, -hy },
				{  hx, -hy },
				{  hx,  hy },
				{ -hx,  hy }
			};
		}

		inline void SetPoints(const std::vector<glm::vec2>& points)
		{
			std::string formatted = Physics2D::FormatPointList(points);
			Points = formatted.empty() ? NameId{} : StringPool::Get().InternName(formatted);
		}

		WE_SCHEMA_BODY(World, ShadowCaster2DComponent, Component)
			WE_SCHEMA_META(Category("Lighting"),
				Doc("2D shadow caster hull that extrudes silhouette shadow geometry away from 2D point lights."))
			WE_FIELD(Offset, Vec2,
				Doc("Local 2D center offset of the shadow hull."));
			WE_FIELD(Size, Vec2,
				Doc("Width and height of the shadow hull when ShapeType is Box (0)."));
			WE_FIELD(Points, Name,
				Doc("Semicolon-separated 2D polygon vertices when ShapeType is Polygon (1)."));
			WE_FIELD(ShadowIntensity, Float, Default(0.85f), Range(0.0f, 1.0f),
				Doc("Shadow occlusion darkness in [0, 1] (1 = completely blocks point light)."));
			WE_FIELD(ExtrusionLength, Float, Default(12.0f), Range(0.5f, 200.0f),
				Doc("Distance in world units to extrude silhouette shadow geometry away from lights."));
			WE_FIELD(ShapeType, UInt8, Default(0),
				Doc("Silhouette shape mode: 0 = Box (uses Size), 1 = Polygon (uses Points)."));
			WE_FIELD(SelfShadows, Bool, Default(false),
				Doc("Whether this entity shadows its own sprite surface."));
			WE_FIELD(CastShadows, Bool, Default(true),
				Doc("Whether this shadow caster actively blocks 2D lights."));
			WE_FIELD(ShowGizmo, Bool, Default(true),
				Doc("Draw silhouette and extruded shadow rays in the editor viewport."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(ShadowCaster2DComponent) == 32, "ShadowCaster2DComponent must be exactly 32 bytes");
	static_assert(std::is_trivially_copyable_v<ShadowCaster2DComponent>, "ShadowCaster2DComponent must be trivially copyable");

	struct FreeformLight2DComponent
	{
		glm::vec4 Color { 1.0f, 1.0f, 0.9f, 1.0f };
		NameId Points;
		float Intensity = 1.0f;
		float FalloffDistance = 0.5f;
		int32_t TargetSortingLayer = 0;
		bool AffectAllLayers = true;
		bool CastShadows = false;
		bool Enabled = true;
		uint8_t _Padding = 0;

		FreeformLight2DComponent() = default;

		inline std::vector<glm::vec2> GetLocalPolygon() const
		{
			if (Points.IsValid())
			{
				auto pts = Physics2D::ParsePointList(StringPool::Get().NameOf(Points));
				if (pts.size() >= 3)
					return pts;
			}
			return {
				{ -1.0f, -0.7f },
				{  1.0f, -0.7f },
				{  0.6f,  0.8f },
				{ -0.6f,  0.8f }
			};
		}

		inline void SetPoints(const std::vector<glm::vec2>& points)
		{
			std::string formatted = Physics2D::FormatPointList(points);
			Points = formatted.empty() ? NameId{} : StringPool::Get().InternName(formatted);
		}

		WE_SCHEMA_BODY(World, FreeformLight2DComponent, Component)
			WE_SCHEMA_META(Category("Lighting"),
				Doc("2D freeform polygon light: Points defines arbitrary polygon vertices in local space."))
			WE_FIELD(Color, Vec4, Color(),
				Doc("Light tint color and multiplier."));
			WE_FIELD(Points, Name,
				Doc("Semicolon-separated list of 2D vertex coordinates in local space."));
			WE_FIELD(Intensity, Float, Default(1.0f),
				Doc("Light luminous power."));
			WE_FIELD(FalloffDistance, Float, Default(0.5f),
				Doc("Feathered edge falloff distance in world meters."));
			WE_FIELD(TargetSortingLayer, Int32, Default(0),
				Doc("Target sorting layer when AffectAllLayers is false."));
			WE_FIELD(AffectAllLayers, Bool, Default(true),
				Doc("Whether this light affects all 2D sorting layers."));
			WE_FIELD(CastShadows, Bool, Default(false),
				Doc("Whether this light interacts with 2D shadow casters."));
			WE_FIELD(Enabled, Bool, Default(true),
				Doc("Whether this 2D freeform light is active."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(FreeformLight2DComponent) == 36, "FreeformLight2DComponent must be exactly 36 bytes");
	static_assert(std::is_trivially_copyable_v<FreeformLight2DComponent>, "FreeformLight2DComponent must be trivially copyable");



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
	struct CapsuleCollider2DComponent
	{
		glm::vec2 Offset { 0.0f, 0.0f };
		float Radius = 0.25f;
		float Length = 0.5f;
		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		uint8_t Direction = 0; // 0 = Vertical, 1 = Horizontal
		bool ShowCollider = true;
		bool IsSensor = false;
		uint8_t Reserved = 0;

		WE_SCHEMA_BODY(World, CapsuleCollider2DComponent, Component)
			WE_SCHEMA_META(Category("Physics/2D"),
				Doc("2D capsule collider simulated by Box2D: Radius and Length define a smoothed cylinder shape ideal for characters."))
			WE_FIELD(Offset, Vec2,
				Doc("Centre offset in local units."));
			WE_FIELD(Radius, Float, Default(0.25f),
				Doc("Radius of the capsule end caps."));
			WE_FIELD(Length, Float, Default(0.5f),
				Doc("Length of the cylindrical body between end cap centers."));
			WE_FIELD(Density, Float, Default(1.0f),
				Doc("Mass per area."));
			WE_FIELD(Friction, Float, Default(0.5f),
				Doc("Surface friction against other colliders."));
			WE_FIELD(Restitution, Float, Default(0.0f),
				Doc("Bounciness."));
			WE_FIELD(Direction, UInt8, Default(0),
				Doc("0 = Vertical, 1 = Horizontal."));
			WE_FIELD(ShowCollider, Bool, Default(true),
				Doc("Draw the physics debug outline."));
			WE_FIELD(IsSensor, Bool, Default(false),
				Doc("Sensor mode (triggers only, no physical collision)."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(CapsuleCollider2DComponent) == 32, "CapsuleCollider2DComponent must be exactly 32 bytes");
	static_assert(std::is_trivially_copyable_v<CapsuleCollider2DComponent>, "CapsuleCollider2DComponent must be trivially copyable");

	struct PolygonCollider2DComponent
	{
		glm::vec2 Offset { 0.0f, 0.0f };
		NameId Points;
		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		bool ShowCollider = true;
		bool IsSensor = false;
		uint8_t _Padding[2] { 0, 0 };

		inline std::vector<glm::vec2> GetPoints() const
		{
			if (!Points.IsValid()) return Physics2D::GetDefaultPolygonPoints();
			auto pts = Physics2D::ParsePointList(StringPool::Get().NameOf(Points));
			return pts.size() >= 3 ? pts : Physics2D::GetDefaultPolygonPoints();
		}
		inline void SetPoints(const std::vector<glm::vec2>& points)
		{
			std::string formatted = Physics2D::FormatPointList(points);
			Points = formatted.empty() ? NameId{} : StringPool::Get().InternName(formatted);
		}

		WE_SCHEMA_BODY(World, PolygonCollider2DComponent, Component)
			WE_SCHEMA_META(Category("Physics/2D"),
				Doc("2D polygon collider simulated by Box2D: Points defines arbitrary polygon vertices in local space."))
			WE_FIELD(Offset, Vec2,
				Doc("Centre offset in local units."));
			WE_FIELD(Points, Name,
				Doc("Semicolon-separated list of 2D vertex coordinates (e.g. -0.5,-0.5;0.5,-0.5;0,0.5)."));
			WE_FIELD(Density, Float, Default(1.0f),
				Doc("Mass per area."));
			WE_FIELD(Friction, Float, Default(0.5f),
				Doc("Surface friction against other colliders."));
			WE_FIELD(Restitution, Float, Default(0.0f),
				Doc("Bounciness."));
			WE_FIELD(ShowCollider, Bool, Default(true),
				Doc("Draw the physics debug outline."));
			WE_FIELD(IsSensor, Bool, Default(false),
				Doc("Sensor mode (triggers only, no physical collision)."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(PolygonCollider2DComponent) == 28, "PolygonCollider2DComponent must be exactly 28 bytes");
	static_assert(std::is_trivially_copyable_v<PolygonCollider2DComponent>, "PolygonCollider2DComponent must be trivially copyable");

	struct ChainCollider2DComponent
	{
		glm::vec2 Offset { 0.0f, 0.0f };
		NameId Points;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		bool Loop = false;
		bool ShowCollider = true;
		bool IsSensor = false;
		uint8_t _Padding[1] { 0 };

		inline std::vector<glm::vec2> GetPoints() const
		{
			if (!Points.IsValid()) return Physics2D::GetDefaultChainPoints();
			auto pts = Physics2D::ParsePointList(StringPool::Get().NameOf(Points));
			return pts.size() >= 2 ? pts : Physics2D::GetDefaultChainPoints();
		}
		inline void SetPoints(const std::vector<glm::vec2>& points)
		{
			std::string formatted = Physics2D::FormatPointList(points);
			Points = formatted.empty() ? NameId{} : StringPool::Get().InternName(formatted);
		}

		WE_SCHEMA_BODY(World, ChainCollider2DComponent, Component)
			WE_SCHEMA_META(Category("Physics/2D"),
				Doc("2D chain/edge collider simulated by Box2D: Points defines connected line segments ideal for terrain contours."))
			WE_FIELD(Offset, Vec2,
				Doc("Centre offset in local units."));
			WE_FIELD(Points, Name,
				Doc("Semicolon-separated list of 2D vertex coordinates (e.g. -2,0;-1,0.5;1,0.5;2,0)."));
			WE_FIELD(Friction, Float, Default(0.5f),
				Doc("Surface friction against other colliders."));
			WE_FIELD(Restitution, Float, Default(0.0f),
				Doc("Bounciness."));
			WE_FIELD(Loop, Bool, Default(false),
				Doc("Whether the chain forms a closed loop."));
			WE_FIELD(ShowCollider, Bool, Default(true),
				Doc("Draw the physics debug outline."));
			WE_FIELD(IsSensor, Bool, Default(false),
				Doc("Sensor mode (triggers only, no physical collision)."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(ChainCollider2DComponent) == 24, "ChainCollider2DComponent must be exactly 24 bytes");
	static_assert(std::is_trivially_copyable_v<ChainCollider2DComponent>, "ChainCollider2DComponent must be trivially copyable");

	struct CompositeCollider2DComponent
	{
		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		uint8_t GeometryType = 0; // 0 = Polygons, 1 = OutlineChain
		bool IsSensor = false;
		bool ShowCollider = true;
		uint8_t _Padding = 0;

		WE_SCHEMA_BODY(World, CompositeCollider2DComponent, Component)
			WE_SCHEMA_META(Category("Physics/2D"),
				Doc("2D composite collider: merges Box and Polygon colliders across this entity and its children into a unified collision body."))
			WE_FIELD(Density, Float, Default(1.0f),
				Doc("Mass density per unit area."));
			WE_FIELD(Friction, Float, Default(0.5f),
				Doc("Surface friction against other colliders."));
			WE_FIELD(Restitution, Float, Default(0.0f),
				Doc("Bounciness restitution coefficient."));
			WE_FIELD(GeometryType, UInt8, Default(0),
				Doc("Composite geometry type: 0 = Polygons, 1 = OutlineChain."));
			WE_FIELD(IsSensor, Bool, Default(false),
				Doc("Sensor mode (triggers only, no physical response)."));
			WE_FIELD(ShowCollider, Bool, Default(true),
				Doc("Draw composite debug wireframe."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(CompositeCollider2DComponent) == 16, "CompositeCollider2DComponent must be exactly 16 bytes");
	static_assert(std::is_trivially_copyable_v<CompositeCollider2DComponent>, "CompositeCollider2DComponent must be trivially copyable");

	struct PlatformEffector2DComponent
	{
		float SurfaceArc = 180.0f;
		float RotationalOffset = 0.0f;
		bool OneWay = true;
		bool UseSideFriction = false;
		bool UseSideBounce = false;
		uint8_t _Padding[5] { 0, 0, 0, 0, 0 };

		WE_SCHEMA_BODY(World, PlatformEffector2DComponent, Component)
			WE_SCHEMA_META(Category("Physics/2D"),
				Doc("2D platform effector: modifies collision behavior to enable one-way jump-through platforms."))
			WE_FIELD(SurfaceArc, Float, Default(180.0f),
				Doc("Angle of the solid surface in degrees (default 180 = top hemisphere)."));
			WE_FIELD(RotationalOffset, Float, Default(0.0f),
				Doc("Rotational offset in degrees from the entity up vector."));
			WE_FIELD(OneWay, Bool, Default(true),
				Doc("Whether one-way pass-through collision is enabled."));
			WE_FIELD(UseSideFriction, Bool, Default(false),
				Doc("Whether friction is applied on side contact."));
			WE_FIELD(UseSideBounce, Bool, Default(false),
				Doc("Whether bounce is applied on side contact."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(PlatformEffector2DComponent) == 16, "PlatformEffector2DComponent must be exactly 16 bytes");
	static_assert(std::is_trivially_copyable_v<PlatformEffector2DComponent>, "PlatformEffector2DComponent must be trivially copyable");

	struct DistanceJoint2DComponent
	{
		entt::entity ConnectedEntity = entt::null;
		glm::vec2 AnchorSelf { 0.0f, 0.0f };
		glm::vec2 AnchorOther { 0.0f, 0.0f };
		float MinDistance = -1.0f;
		float MaxDistance = -1.0f;
		float Hertz = 0.0f;
		float DampingRatio = 0.7f;
		bool EnableLimit = false;
		bool EnableSpring = false;
		bool EnableCollision = false;
		uint8_t _Padding[1] { 0 };

		DistanceJoint2DComponent() = default;

		WE_SCHEMA_BODY(World, DistanceJoint2DComponent, Component)
			WE_SCHEMA_META(Category("Physics/2D"),
				Doc("2D distance joint: constrains distance between two 2D rigid bodies, with optional min/max distance limits and spring-damper suspension."))
			WE_FIELD(ConnectedEntity, UInt64, Entity32,
				Doc("The target entity to connect with (must have a 2D RigidBody)."));
			WE_FIELD(AnchorSelf, Vec2,
				Doc("Anchor point in this entity local space."));
			WE_FIELD(AnchorOther, Vec2,
				Doc("Anchor point in the connected entity local space."));
			WE_FIELD(MinDistance, Float, 
				Doc("Minimum allowed distance; negative = derived from initial separation."));
			WE_FIELD(MaxDistance, Float, 
				Doc("Maximum allowed distance; negative = derived from initial separation."));
			WE_FIELD(Hertz, Float, Default(0.0f), Range(0.0f, 60.0f),
				Doc("Spring frequency in Hz (0 = rigid rod, > 0 = springy soft constraint)."));
			WE_FIELD(DampingRatio, Float, Default(0.7f), Range(0.0f, 10.0f),
				Doc("Spring damping ratio (0 = no damping, 1 = critical damping)."));
			WE_FIELD(EnableLimit, Bool, Default(false),
				Doc("Whether MinDistance and MaxDistance clamping is active."));
			WE_FIELD(EnableSpring, Bool, Default(false),
				Doc("Whether spring-damper stiffness is active."));
			WE_FIELD(EnableCollision, Bool, Default(false),
				Doc("Whether the two connected bodies can collide with each other."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(DistanceJoint2DComponent) == 40, "DistanceJoint2DComponent must be exactly 40 bytes");
	static_assert(std::is_trivially_copyable_v<DistanceJoint2DComponent>, "DistanceJoint2DComponent must be trivially copyable");

	struct RevoluteJoint2DComponent
	{
		entt::entity ConnectedEntity = entt::null;
		glm::vec2 AnchorSelf { 0.0f, 0.0f };
		glm::vec2 AnchorOther { 0.0f, 0.0f };
		float LowerAngle = 0.0f;
		float UpperAngle = 0.0f;
		float MotorSpeed = 0.0f;
		float MaxMotorTorque = 10.0f;
		bool EnableLimit = false;
		bool EnableMotor = false;
		bool EnableCollision = false;
		uint8_t _Padding[1] { 0 };

		RevoluteJoint2DComponent() = default;

		WE_SCHEMA_BODY(World, RevoluteJoint2DComponent, Component)
			WE_SCHEMA_META(Category("Physics/2D"),
				Doc("2D revolute (hinge/pin) joint: allows rotational motion around a shared anchor point with angle limits and rotational motor."))
			WE_FIELD(ConnectedEntity, UInt64, Entity32,
				Doc("The target entity to connect with (must have a 2D RigidBody)."));
			WE_FIELD(AnchorSelf, Vec2,
				Doc("Pin anchor point in this entity local space."));
			WE_FIELD(AnchorOther, Vec2,
				Doc("Pin anchor point in the connected entity local space."));
			WE_FIELD(LowerAngle, Float, Default(0.0f),
				Doc("Lower rotational angle limit in degrees."));
			WE_FIELD(UpperAngle, Float, Default(0.0f),
				Doc("Upper rotational angle limit in degrees."));
			WE_FIELD(MotorSpeed, Float, Default(0.0f),
				Doc("Target motor rotational speed in degrees per second."));
			WE_FIELD(MaxMotorTorque, Float, Default(10.0f), Range(0.0f, 10000.0f),
				Doc("Maximum motor torque in N*m."));
			WE_FIELD(EnableLimit, Bool, Default(false),
				Doc("Whether angle limits are enforced."));
			WE_FIELD(EnableMotor, Bool, Default(false),
				Doc("Whether the rotational motor is active."));
			WE_FIELD(EnableCollision, Bool, Default(false),
				Doc("Whether the two connected bodies can collide with each other."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(RevoluteJoint2DComponent) == 40, "RevoluteJoint2DComponent must be exactly 40 bytes");
	static_assert(std::is_trivially_copyable_v<RevoluteJoint2DComponent>, "RevoluteJoint2DComponent must be trivially copyable");

	struct PrismaticJoint2DComponent
	{
		entt::entity ConnectedEntity = entt::null;
		glm::vec2 AnchorSelf { 0.0f, 0.0f };
		glm::vec2 AnchorOther { 0.0f, 0.0f };
		glm::vec2 Axis { 1.0f, 0.0f };
		float LowerTranslation = -1.0f;
		float UpperTranslation = 1.0f;
		float MotorSpeed = 0.0f;
		float MaxMotorForce = 50.0f;
		bool EnableLimit = false;
		bool EnableMotor = false;
		bool EnableCollision = false;
		uint8_t _Padding[1] { 0 };

		PrismaticJoint2DComponent() = default;

		WE_SCHEMA_BODY(World, PrismaticJoint2DComponent, Component)
			WE_SCHEMA_META(Category("Physics/2D"),
				Doc("2D prismatic (slider) joint: allows linear motion along a specified axis with translation limits and linear motor."))
			WE_FIELD(ConnectedEntity, UInt64, Entity32,
				Doc("The target entity to connect with (must have a 2D RigidBody)."));
			WE_FIELD(AnchorSelf, Vec2,
				Doc("Anchor point in this entity local space."));
			WE_FIELD(AnchorOther, Vec2,
				Doc("Anchor point in the connected entity local space."));
			WE_FIELD(Axis, Vec2,
				Doc("Sliding translation axis in this entity local space."));
			WE_FIELD(LowerTranslation, Float,
				Doc("Lower translation limit in meters."));
			WE_FIELD(UpperTranslation, Float, Default(1.0f),
				Doc("Upper translation limit in meters."));
			WE_FIELD(MotorSpeed, Float, Default(0.0f),
				Doc("Target linear motor speed in meters per second."));
			WE_FIELD(MaxMotorForce, Float, Default(50.0f), Range(0.0f, 10000.0f),
				Doc("Maximum linear motor force in Newtons."));
			WE_FIELD(EnableLimit, Bool, Default(false),
				Doc("Whether translation limits are enforced."));
			WE_FIELD(EnableMotor, Bool, Default(false),
				Doc("Whether the linear motor is active."));
			WE_FIELD(EnableCollision, Bool, Default(false),
				Doc("Whether the two connected bodies can collide with each other."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(PrismaticJoint2DComponent) == 48, "PrismaticJoint2DComponent must be exactly 48 bytes");
	static_assert(std::is_trivially_copyable_v<PrismaticJoint2DComponent>, "PrismaticJoint2DComponent must be trivially copyable");

	struct WheelJoint2DComponent
	{
		entt::entity ConnectedEntity = entt::null;
		glm::vec2 AnchorSelf { 0.0f, 0.0f };
		glm::vec2 AnchorOther { 0.0f, 0.0f };
		glm::vec2 SuspensionAxis { 0.0f, 1.0f };
		float Hertz = 4.0f;
		float DampingRatio = 0.7f;
		float LowerTranslation = -0.2f;
		float UpperTranslation = 0.2f;
		float MotorSpeed = 0.0f;
		float MaxMotorTorque = 20.0f;
		bool EnableLimit = true;
		bool EnableMotor = false;
		bool EnableCollision = false;
		uint8_t _Padding[1] { 0 };

		WheelJoint2DComponent() = default;

		WE_SCHEMA_BODY(World, WheelJoint2DComponent, Component)
			WE_SCHEMA_META(Category("Physics/2D"),
				Doc("2D wheel joint: provides vehicle wheel suspension with spring-damper suspension axis, travel limits, and rotational drive motor."))
			WE_FIELD(ConnectedEntity, UInt64, Entity32,
				Doc("The target entity to connect with (must have a 2D RigidBody)."));
			WE_FIELD(AnchorSelf, Vec2,
				Doc("Wheel mount anchor point in this entity local space."));
			WE_FIELD(AnchorOther, Vec2,
				Doc("Wheel axle anchor point in the connected entity local space."));
			WE_FIELD(SuspensionAxis, Vec2,
				Doc("Suspension shock-absorber axis in this entity local space."));
			WE_FIELD(Hertz, Float, Default(4.0f), Range(0.0f, 60.0f),
				Doc("Suspension spring frequency in Hz."));
			WE_FIELD(DampingRatio, Float, Default(0.7f), Range(0.0f, 10.0f),
				Doc("Suspension spring damping ratio."));
			WE_FIELD(LowerTranslation, Float,
				Doc("Lower suspension travel limit in meters."));
			WE_FIELD(UpperTranslation, Float, Default(0.2f),
				Doc("Upper suspension travel limit in meters."));
			WE_FIELD(MotorSpeed, Float, Default(0.0f),
				Doc("Wheel drive motor target speed in degrees per second."));
			WE_FIELD(MaxMotorTorque, Float, Default(20.0f), Range(0.0f, 10000.0f),
				Doc("Maximum wheel motor torque in N*m."));
			WE_FIELD(EnableLimit, Bool, Default(true),
				Doc("Whether suspension travel limits are enforced."));
			WE_FIELD(EnableMotor, Bool, Default(false),
				Doc("Whether wheel drive motor is active."));
			WE_FIELD(EnableCollision, Bool, Default(false),
				Doc("Whether the two connected bodies can collide with each other."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(WheelJoint2DComponent) == 56, "WheelJoint2DComponent must be exactly 56 bytes");
	static_assert(std::is_trivially_copyable_v<WheelJoint2DComponent>, "WheelJoint2DComponent must be trivially copyable");

	struct AreaEffector2DComponent
	{
		glm::vec2 ForceDirection { 0.0f, 1.0f };
		float ForceMagnitude = 10.0f;
		float LinearDrag = 0.5f;
		float AngularDrag = 0.5f;
		uint8_t ForceMode = 0; // 0 = Directional, 1 = PointAttraction, 2 = PointRepulsion
		bool UseGlobalCoordinates = false;
		bool AffectsInertia = false;
		bool Enabled = true;
		uint8_t _Padding[4] { 0, 0, 0, 0 };

		AreaEffector2DComponent() = default;

		WE_SCHEMA_BODY(World, AreaEffector2DComponent, Component)
			WE_SCHEMA_META(Category("Physics/2D"),
				Doc("2D area effector: applies directional wind forces or point attraction/repulsion to dynamic bodies inside this sensor volume."))
			WE_FIELD(ForceDirection, Vec2,
				Doc("Direction vector of the applied force when in Directional mode."));
			WE_FIELD(ForceMagnitude, Float, Default(10.0f), Range(0.0f, 10000.0f),
				Doc("Magnitude of the force in Newtons (or acceleration if AffectsInertia is true)."));
			WE_FIELD(LinearDrag, Float, Default(0.5f), Range(0.0f, 100.0f),
				Doc("Linear air resistance/drag applied to bodies inside the volume."));
			WE_FIELD(AngularDrag, Float, Default(0.5f), Range(0.0f, 100.0f),
				Doc("Rotational air resistance/drag applied to bodies inside the volume."));
			WE_FIELD(ForceMode, UInt8, Default(0),
				Doc("0 = Directional (constant wind vector), 1 = PointAttraction (towards center), 2 = PointRepulsion (away from center)."));
			WE_FIELD(UseGlobalCoordinates, Bool, Default(false),
				Doc("Whether ForceDirection is in world coordinates (true) or rotates with the entity (false)."));
			WE_FIELD(AffectsInertia, Bool, Default(false),
				Doc("Whether force scales with body mass (acceleration mode) or is constant."));
			WE_FIELD(Enabled, Bool, Default(true),
				Doc("Whether this area effector is active."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(AreaEffector2DComponent) == 28, "AreaEffector2DComponent must be exactly 28 bytes");
	static_assert(std::is_trivially_copyable_v<AreaEffector2DComponent>, "AreaEffector2DComponent must be trivially copyable");

	struct BuoyancyEffector2DComponent
	{
		float Density = 2.0f;
		float SurfaceLevel = 0.0f;
		float LinearDrag = 2.5f;
		float AngularDrag = 1.5f;
		glm::vec2 FlowVelocity { 0.0f, 0.0f };
		bool Enabled = true;
		uint8_t _Padding[7] { 0, 0, 0, 0, 0, 0, 0 };

		BuoyancyEffector2DComponent() = default;

		WE_SCHEMA_BODY(World, BuoyancyEffector2DComponent, Component)
			WE_SCHEMA_META(Category("Physics/2D"),
				Doc("2D buoyancy effector: simulates fluid submersion, counter-gravity buoyancy, and fluid drag resistance."))
			WE_FIELD(Density, Float, Default(2.0f), Range(0.01f, 100.0f),
				Doc("Fluid density relative to body density (higher values cause stronger upward buoyancy floatation)."));
			WE_FIELD(SurfaceLevel, Float, Default(0.0f),
				Doc("Water surface Y height in local coordinates above the entity center."));
			WE_FIELD(LinearDrag, Float, Default(2.5f), Range(0.0f, 100.0f),
				Doc("Fluid linear viscous drag coefficient dampening body velocity."));
			WE_FIELD(AngularDrag, Float, Default(1.5f), Range(0.0f, 100.0f),
				Doc("Fluid rotational drag coefficient dampening spin."));
			WE_FIELD(FlowVelocity, Vec2,
				Doc("Current/river flow velocity vector dragging submerged objects."));
			WE_FIELD(Enabled, Bool, Default(true),
				Doc("Whether this buoyancy effector is active."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(BuoyancyEffector2DComponent) == 32, "BuoyancyEffector2DComponent must be exactly 32 bytes");
	static_assert(std::is_trivially_copyable_v<BuoyancyEffector2DComponent>, "BuoyancyEffector2DComponent must be trivially copyable");

	struct SurfaceEffector2DComponent
	{
		float Speed = 3.0f;
		float ForceScale = 1.0f;
		bool Enabled = true;
		uint8_t _Padding[7] { 0, 0, 0, 0, 0, 0, 0 };

		SurfaceEffector2DComponent() = default;

		WE_SCHEMA_BODY(World, SurfaceEffector2DComponent, Component)
			WE_SCHEMA_META(Category("Physics/2D"),
				Doc("2D surface effector: conveyor belt that propels contacting dynamic bodies along the surface tangent."))
			WE_FIELD(Speed, Float, Default(3.0f), Range(0.0f, 100.0f),
				Doc("Tangent speed in meters per second pushing bodies touching this surface."));
			WE_FIELD(ForceScale, Float, Default(1.0f), Range(0.0f, 10.0f),
				Doc("Scale factor for conveyor friction drive force."));
			WE_FIELD(Enabled, Bool, Default(true),
				Doc("Whether this surface effector is active."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(SurfaceEffector2DComponent) == 16, "SurfaceEffector2DComponent must be exactly 16 bytes");
	static_assert(std::is_trivially_copyable_v<SurfaceEffector2DComponent>, "SurfaceEffector2DComponent must be trivially copyable");


	// P1b D6:3D 物理(Jolt)组件。字段 id 全部显式钉住("RB3D…"/"…3D…" ASCII):
	// 组件一旦写进 .entity 存档就不能再改 id,否则旧场景迁移语义会漂移,--check 门禁也要求
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
			WE_SCHEMA_META(Category("Physics/3D"),
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

	// Level 2.0 数据层归属。实体属于哪些关卡数据层(Base_World / Day / Night / 剧情分支…),
	// 位掩码与关卡包 manifest 的 DataLayers[].Mask 对应。
	//
	// 场景另持"当前激活掩码"(Scene::SetActiveDataLayers),渲染与脚本按 `Mask & Active` 剔除 ——
	// 昼夜/战损这类"同一空间多套内容"因此是**真实可编辑的数据**(component 随 `.entity` 落盘、
	// 在属性面板里可改),而不是只在面板上翻一个不起作用的开关。
	// 大世界空间分区特定区域覆盖卷 (Partition Volume)
	// 用于繁华城镇、高密建筑群、地下矿洞等特定区域的局部细切分块(如 16m/32m)与流送半径覆盖。
	struct PartitionVolumeComponent
	{
		glm::vec3 HalfExtents { 50.0f, 25.0f, 50.0f }; // 半长宽高（范围：长100m, 高50m, 宽100m）
		uint32_t OverrideCellSize { 16 };             // 局部特化分块边长(米): 16, 32 等
		float OverrideStreamingRadius { 64.0f };      // 局部流送加载半径(米)
		bool Active { true };                         // 是否启用该区域覆盖
		uint8_t _Padding[3] { 0, 0, 0 };              // 补齐 24B

		PartitionVolumeComponent() = default;
		PartitionVolumeComponent(const glm::vec3& extents, uint32_t cellSize = 16, float radius = 64.0f)
			: HalfExtents(extents), OverrideCellSize(cellSize), OverrideStreamingRadius(radius), Active(true) {}

		bool Contains(const glm::vec3& worldPos, const glm::vec3& volumeCenter) const
		{
			if (!Active) return false;
			const glm::vec3 delta = glm::abs(worldPos - volumeCenter);
			return delta.x <= HalfExtents.x && delta.y <= HalfExtents.y && delta.z <= HalfExtents.z;
		}

		WE_SCHEMA_BODY(World, PartitionVolumeComponent, Component)
			WE_SCHEMA_META(Category("Environment"),
				Doc("Spatial partition volume that overrides grid cell size and streaming radius for entities inside it."))
			WE_FIELD(HalfExtents, Vec3, Id(0x50564C48414C4658),
				Doc("Half size of the partition volume box in world meters."));
			WE_FIELD(OverrideCellSize, UInt32, Id(0x50564C43454C4C53),
				Doc("Local cell size in meters (e.g. 16m for dense cities)."));
			WE_FIELD(OverrideStreamingRadius, Float, Id(0x50564C5354524144),
				Doc("Local streaming load radius in meters."));
			WE_FIELD(Active, Bool, Id(0x50564C4143544956),
				Doc("Whether this partition volume is currently active."));
		WE_SCHEMA_END
	};

	struct LevelEntityComponent
	{
		uint32_t DataLayerMask { 1 };   // 1 = Base_World(常驻层)

		LevelEntityComponent() = default;
		explicit LevelEntityComponent(uint32_t mask) : DataLayerMask(mask) {}

		WE_SCHEMA_BODY(World, LevelEntityComponent, Component)
			WE_SCHEMA_META(Category("Scene"),
				Doc("Level 2.0 data-layer membership: which level data layers this entity belongs to."),
				NotInPrefab())
			WE_FIELD(DataLayerMask, UInt32, Id(0x4C564C4C41594552), Default(1),
				Group("Level"),
				Doc("Bitmask matching the level package manifest DataLayers[].Mask; 0 = always visible."));
		WE_SCHEMA_END
	};


	struct AudioSourceComponent
	{
		AssetRef Sound;
		float Volume = 1.0f;
		float Pitch = 1.0f;
		float MinDistance = 1.0f;
		float MaxDistance = 25.0f;
		bool Spatial3D = true;
		bool Looping = false;
		bool PlayOnAwake = true;
		uint8_t _Padding = 0;

		AudioSourceComponent() = default;

		WE_SCHEMA_BODY(World, AudioSourceComponent, Component)
			WE_SCHEMA_META(Category("Audio"),
				Doc("3D/2D Audio Source: plays a .wsound event or audio clip with distance attenuation and spatial panning."))
			WE_FIELD(Sound, Asset, Of("SoundEvent"),
				Doc("Sound event or audio asset to play."));
			WE_FIELD(Volume, Float, Range(0.0f, 4.0f),
				Doc("Playback volume gain multiplier (1.0 = 0dB)."));
			WE_FIELD(Pitch, Float, Range(0.1f, 4.0f),
				Doc("Playback pitch multiplier (1.0 = normal, 2.0 = +1 octave)."));
			WE_FIELD(MinDistance, Float, Range(0.1f, 1000.0f),
				Doc("Minimum distance in meters where sound plays at full volume."));
			WE_FIELD(MaxDistance, Float, Range(0.5f, 5000.0f),
				Doc("Maximum distance in meters where sound fades out completely."));
			WE_FIELD(Spatial3D, Bool,
				Doc("Enable 3D spatial positioning and distance attenuation."));
			WE_FIELD(Looping, Bool,
				Doc("Whether the sound repeats continuously."));
			WE_FIELD(PlayOnAwake, Bool,
				Doc("Automatically start playing when scene starts."));
		WE_SCHEMA_END
	};

	struct AudioListenerComponent
	{
		float VolumeScale = 1.0f;
		bool Active = true;
		uint8_t _Padding[3] { 0, 0, 0 };

		AudioListenerComponent() = default;

		WE_SCHEMA_BODY(World, AudioListenerComponent, Component)
			WE_SCHEMA_META(Category("Audio"),
				Doc("Audio Listener: receives 3D spatial sound from the scene (usually attached to the Camera)."))
			WE_FIELD(VolumeScale, Float, Range(0.0f, 2.0f),
				Doc("Master volume scaling factor for this listener."));
			WE_FIELD(Active, Bool,
				Doc("Whether this listener actively receives sound."));
		WE_SCHEMA_END
	};

	enum class PhysMatCombineMode : uint8_t
	{
		Average = 0,
		Minimum = 1,
		Multiply = 2,
		Maximum = 3
	};
	WE_ENUM_SCHEMA(World, PhysMatCombineMode, UInt8)
		WE_ENUM_VALUE(Average);
		WE_ENUM_VALUE(Minimum);
		WE_ENUM_VALUE(Multiply);
		WE_ENUM_VALUE(Maximum);
	WE_ENUM_END

	struct PhysicsMaterialComponent
	{
		AssetRef Material;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		PhysMatCombineMode FrictionCombine = PhysMatCombineMode::Average;
		PhysMatCombineMode RestitutionCombine = PhysMatCombineMode::Average;
		uint8_t _Padding[6] { 0, 0, 0, 0, 0, 0 };

		PhysicsMaterialComponent() = default;

		WE_SCHEMA_BODY(World, PhysicsMaterialComponent, Component)
			WE_SCHEMA_META(Category("Physics/3D"),
				Doc("Physics Material properties for friction, bounciness and combine algorithms."))
			WE_FIELD(Material, Asset, Of("PhysicsMaterial"),
				Doc("Path to .wphysmat physical material asset."));
			WE_FIELD(Friction, Float, Range(0.0f, 2.0f),
				Doc("Surface friction coefficient."));
			WE_FIELD(Restitution, Float, Range(0.0f, 1.0f),
				Doc("Surface elasticity / bounciness coefficient."));
			WE_FIELD(FrictionCombine, Enum, Of(PhysMatCombineMode),
				Doc("Friction combine mode when contacting another surface."));
			WE_FIELD(RestitutionCombine, Enum, Of(PhysMatCombineMode),
				Doc("Restitution combine mode when contacting another surface."));
		WE_SCHEMA_END
	};

	struct CharacterControllerComponent
	{
		glm::vec3 Velocity { 0.0f };             // 12B

		float StepOffset = 0.3f;                 // 4B
		float SlopeLimitDeg = 45.0f;             // 4B
		float SkinWidth = 0.02f;                 // 4B
		float CoyoteDuration = 0.12f;            // 4B
		float JumpBufferDuration = 0.10f;        // 4B
		float CoyoteTimer = 0.0f;                // 4B
		float JumpBufferTimer = 0.0f;            // 4B
		float GravityScale = 1.0f;               // 4B
		bool Grounded = false;                   // 1B
		bool IsSliding = false;                  // 1B
		bool LockZPlane = false;                 // 1B
		uint8_t CollisionFlags = 0;              // 1B
		uint32_t CollisionMask = 0xFFFFFFFF;     // 4B
		uint32_t CollisionLayer = 1;             // 4B
		uint8_t _Padding[8] { 0 };               // 8B

		CharacterControllerComponent() = default;

		WE_SCHEMA_BODY(World, CharacterControllerComponent, Component)
			WE_SCHEMA_META(Category("Physics/3D"),
				Doc("Kinematic Character Controller for unified 2D and 3D Move-and-Slide, slope limiting, and jump buffering."))
			WE_FIELD(Velocity, Vec3,
				Doc("Current movement velocity."));
			WE_FIELD(StepOffset, Float, Range(0.0f, 1.0f),
				Doc("Maximum vertical step-up obstacle height."));
			WE_FIELD(SlopeLimitDeg, Float, Range(0.0f, 89.0f),
				Doc("Maximum walkable slope angle in degrees."));
			WE_FIELD(SkinWidth, Float, Range(0.001f, 0.1f),
				Doc("Small margin outward to prevent collision tunneling."));
			WE_FIELD(CoyoteDuration, Float, Range(0.0f, 0.5f),
				Doc("Coyote time window (seconds) allowing jumping after leaving a ledge."));
			WE_FIELD(JumpBufferDuration, Float, Range(0.0f, 0.5f),
				Doc("Jump buffer window (seconds) queuing jump requests before landing."));
			WE_FIELD(GravityScale, Float, Range(0.0f, 5.0f),
				Doc("Gravity scale factor."));
			WE_FIELD(Grounded, Bool,
				Doc("True if currently standing firmly on the ground."));
			WE_FIELD(IsSliding, Bool,
				Doc("True if currently sliding down a steep slope exceeding SlopeLimitDeg."));
			WE_FIELD(LockZPlane, Bool,
				Doc("True if movement is constrained to 2D XY plane."));
		WE_SCHEMA_END
	};

	struct VirtualCameraComponent
	{
		glm::vec3 FollowOffset { 0.0f, 0.0f, 10.0f };   // 12B
		glm::vec3 Damping { 0.1f, 0.1f, 0.1f };          // 12B
		glm::vec2 DeadZone { 0.1f, 0.1f };              // 8B
		float LookaheadTime = 0.0f;                     // 4B
		float ScreenShakeTrauma = 0.0f;                 // 4B
		float ScreenShakeFrequency = 15.0f;             // 4B
		float ScreenShakeDecay = 1.0f;                  // 4B
		int32_t Priority = 10;                          // 4B
		uint32_t FollowTarget = 0;                      // 4B
		uint16_t Flags = 1;                             // 2B (bit 0: Active, bit 1: Confine, bit 2: LockZ)
		uint8_t _Padding[6] { 0 };                      // 6B

		VirtualCameraComponent() = default;

		WE_SCHEMA_BODY(World, VirtualCameraComponent, Component)
			WE_SCHEMA_META(Category("Camera"),
				Doc("Cinemachine-style virtual camera for smooth target tracking, deadzone, lookahead, and screen shake."))
			WE_FIELD(FollowOffset, Vec3,
				Doc("Offset from target position."));
			WE_FIELD(Damping, Vec3,
				Doc("Smooth damping factor for tracking."));
			WE_FIELD(DeadZone, Vec2,
				Doc("Normalized screen-space deadzone half-extents."));
			WE_FIELD(LookaheadTime, Float, Range(0.0f, 2.0f),
				Doc("Target velocity lookahead prediction time in seconds."));
			WE_FIELD(ScreenShakeTrauma, Float, Range(0.0f, 1.0f),
				Doc("Current screen shake trauma value [0, 1]."));
			WE_FIELD(ScreenShakeFrequency, Float, Range(1.0f, 50.0f),
				Doc("Screen shake frequency in Hz."));
			WE_FIELD(ScreenShakeDecay, Float, Range(0.1f, 10.0f),
				Doc("Rate at which trauma decays per second."));
			WE_FIELD(Priority, Int32,
				Doc("Virtual camera activation priority."));
		WE_SCHEMA_END
	};

	struct PostProcessComponent
	{
		glm::vec4 OutlineColor { 1.0f, 1.0f, 0.0f, 1.0f }; // 16B
		float FlashWhiteIntensity = 0.0f;                  // 4B
		float FlashWhiteDecay = 5.0f;                      // 4B
		float OutlineWidth = 0.0f;                         // 4B
		float DissolveProgress = 0.0f;                     // 4B
		float BloomIntensity = 0.0f;                       // 4B
		float BloomThreshold = 1.0f;                       // 4B
		float VignetteIntensity = 0.0f;                    // 4B
		float ChromaticAberration = 0.0f;                  // 4B
		bool Enabled = true;                               // 1B
		bool LutEnabled = false;                           // 1B
		uint8_t _Padding[6] { 0 };                        // 10B

		PostProcessComponent() = default;

		WE_SCHEMA_BODY(World, PostProcessComponent, Component)
			WE_SCHEMA_META(Category("Effects"),
				Doc("Fullscreen post-processing volume settings: hit flash, outline, dissolve, bloom, and color grading."))
			WE_FIELD(OutlineColor, Vec4,
				Doc("Outline silhouette edge color."));
			WE_FIELD(FlashWhiteIntensity, Float, Range(0.0f, 1.0f),
				Doc("Flash-white hit reaction intensity."));
			WE_FIELD(FlashWhiteDecay, Float, Range(0.1f, 20.0f),
				Doc("Flash-white decay speed per second."));
			WE_FIELD(OutlineWidth, Float, Range(0.0f, 10.0f),
				Doc("Silhouette outline width in pixels."));
			WE_FIELD(DissolveProgress, Float, Range(0.0f, 1.0f),
				Doc("Dissolve effect cutoff progress [0, 1]."));
			WE_FIELD(BloomIntensity, Float, Range(0.0f, 5.0f),
				Doc("Dual-Kawase bloom glow intensity."));
			WE_FIELD(BloomThreshold, Float, Range(0.0f, 5.0f),
				Doc("Luminance threshold for bloom extraction."));
			WE_FIELD(VignetteIntensity, Float, Range(0.0f, 1.0f),
				Doc("Vignette border darkening intensity."));
			WE_FIELD(ChromaticAberration, Float, Range(0.0f, 1.0f),
				Doc("Radial chromatic aberration color fringing amount."));
			WE_FIELD(Enabled, Bool,
				Doc("Master toggle for post-processing volume."));
		WE_SCHEMA_END
	};
	struct TerrainComponent
	{
		AssetRef TerrainAsset;                           // 16B (offset 0)
		float CellSize { 1.0f };                         // 4B  (offset 16)
		float HeightScale { 1.0f };                      // 4B  (offset 20)
		float MinHeight { 0.0f };                        // 4B  (offset 24)
		float MaxHeight { 256.0f };                      // 4B  (offset 28)
		uint32_t ChunkResolution { 64 };                 // 4B  (offset 32)
		float LodDistanceRatio { 1.5f };                 // 4B  (offset 36)
		uint32_t LayerCount { 4 };                       // 4B  (offset 40)
		uint8_t CastShadow { 1 };                        // 1B  (offset 44)
		uint8_t ReceiveShadow { 1 };                     // 1B  (offset 45)
		uint8_t HoleCutoutEnabled { 1 };                 // 1B  (offset 46)
		uint8_t EnableCollision { 1 };                   // 1B  (offset 47)

		TerrainComponent() = default;

		WE_SCHEMA_BODY(World, TerrainComponent, Component)
			WE_SCHEMA_META(Category("Environment"),
				Doc("3D heightfield terrain component with LOD chunks, multi-layer splat blending and collision."))
			WE_FIELD(TerrainAsset, Asset, Of("Terrain"),
				Doc("Reference to the .wterrain data asset containing heightfield and splatmap data."));
			WE_FIELD(CellSize, Float, Range(0.1f, 100.0f),
				Doc("World space grid cell spacing in units."));
			WE_FIELD(HeightScale, Float, Range(0.01f, 100.0f),
				Doc("Vertical scaling multiplier applied to heights."));
			WE_FIELD(MinHeight, Float,
				Doc("Minimum terrain elevation bound."));
			WE_FIELD(MaxHeight, Float,
				Doc("Maximum terrain elevation bound."));
			WE_FIELD(ChunkResolution, UInt32, Range(8, 256),
				Doc("Vertex resolution per terrain chunk quad (e.g. 64x64)."));
			WE_FIELD(LodDistanceRatio, Float, Range(0.5f, 10.0f),
				Doc("Distance transition ratio for chunk CDLOD transitions."));
			WE_FIELD(LayerCount, UInt32, Range(1, 4),
				Doc("Number of active splat material layers (1 to 4)."));
			WE_FIELD(CastShadow, UInt8, Default(1),
				Doc("Whether the terrain casts shadows into the scene."));
			WE_FIELD(ReceiveShadow, UInt8, Default(1),
				Doc("Whether the terrain receives shadow maps."));
			WE_FIELD(HoleCutoutEnabled, UInt8, Default(1),
				Doc("Enable cave/tunnel hole cutout rendering."));
			WE_FIELD(EnableCollision, UInt8, Default(1),
				Doc("Enable 3D heightfield physics collision."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(TerrainComponent) == 48, "TerrainComponent must be exactly 48 bytes");
	static_assert(std::is_trivially_copyable_v<TerrainComponent>, "TerrainComponent must be trivially copyable");
	static_assert(offsetof(TerrainComponent, TerrainAsset) == 0, "TerrainAsset offset mismatch");
	static_assert(offsetof(TerrainComponent, CellSize) == 16, "CellSize offset mismatch");
	static_assert(offsetof(TerrainComponent, HeightScale) == 20, "HeightScale offset mismatch");
	static_assert(offsetof(TerrainComponent, MinHeight) == 24, "MinHeight offset mismatch");
	static_assert(offsetof(TerrainComponent, MaxHeight) == 28, "MaxHeight offset mismatch");
	static_assert(offsetof(TerrainComponent, ChunkResolution) == 32, "ChunkResolution offset mismatch");
	static_assert(offsetof(TerrainComponent, LodDistanceRatio) == 36, "LodDistanceRatio offset mismatch");
	static_assert(offsetof(TerrainComponent, LayerCount) == 40, "LayerCount offset mismatch");
	static_assert(offsetof(TerrainComponent, CastShadow) == 44, "CastShadow offset mismatch");
	static_assert(offsetof(TerrainComponent, ReceiveShadow) == 45, "ReceiveShadow offset mismatch");
	static_assert(offsetof(TerrainComponent, HoleCutoutEnabled) == 46, "HoleCutoutEnabled offset mismatch");
	static_assert(offsetof(TerrainComponent, EnableCollision) == 47, "EnableCollision offset mismatch");


	struct FoliageComponent
	{
		AssetRef FoliageAsset;                            // 16B (offset 0)
		float MaxCullDistance { 150.0f };                 // 4B  (offset 16)
		float FadeRange { 20.0f };                        // 4B  (offset 20)
		float WindStrength { 1.0f };                      // 4B  (offset 24)
		float WindFrequency { 1.0f };                     // 4B  (offset 28)
		uint32_t InstanceCount { 0 };                     // 4B  (offset 32)
		uint8_t CastShadow { 1 };                         // 1B  (offset 36)
		uint8_t ReceiveShadow { 1 };                      // 1B  (offset 37)
		uint8_t AlignToNormal { 1 };                      // 1B  (offset 38)
		uint8_t EnableWind { 1 };                         // 1B  (offset 39)
		uint8_t _Padding[8] { 0 };                        // 8B  (offset 40)

		FoliageComponent() = default;

		WE_SCHEMA_BODY(World, FoliageComponent, Component)
			WE_SCHEMA_META(Category("Environment"),
				Doc("GPU-instanced foliage and vegetation scatter cluster."))
			WE_FIELD(FoliageAsset, Asset, Of("Foliage"),
				Doc("Reference to the .wfoliage asset."));
			WE_FIELD(MaxCullDistance, Float, Range(10.0f, 1000.0f), Default(150.0f),
				Doc("Maximum view distance in meters before foliage is culled."));
			WE_FIELD(FadeRange, Float, Range(1.0f, 100.0f), Default(20.0f),
				Doc("Transition range for screen-door dither distance fade."));
			WE_FIELD(WindStrength, Float, Range(0.0f, 10.0f), Default(1.0f),
				Doc("Global wind bend strength multiplier."));
			WE_FIELD(WindFrequency, Float, Range(0.1f, 10.0f), Default(1.0f),
				Doc("Wind wave oscillation frequency."));
			WE_FIELD(InstanceCount, UInt32, Default(0),
				Doc("Total number of active foliage instances."));
			WE_FIELD(CastShadow, UInt8, Default(1),
				Doc("Whether foliage instances cast shadows."));
			WE_FIELD(ReceiveShadow, UInt8, Default(1),
				Doc("Whether foliage instances receive shadows."));
			WE_FIELD(AlignToNormal, UInt8, Default(1),
				Doc("Whether foliage rotates to align with ground/surface normals."));
			WE_FIELD(EnableWind, UInt8, Default(1),
				Doc("Whether wind sway vertex animation is active."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(FoliageComponent) == 48, "FoliageComponent must be exactly 48 bytes");
	static_assert(std::is_trivially_copyable_v<FoliageComponent>, "FoliageComponent must be trivially copyable");
	static_assert(offsetof(FoliageComponent, FoliageAsset) == 0, "FoliageAsset offset mismatch");
	static_assert(offsetof(FoliageComponent, MaxCullDistance) == 16, "MaxCullDistance offset mismatch");
	static_assert(offsetof(FoliageComponent, FadeRange) == 20, "FadeRange offset mismatch");
	static_assert(offsetof(FoliageComponent, WindStrength) == 24, "WindStrength offset mismatch");
	static_assert(offsetof(FoliageComponent, WindFrequency) == 28, "WindFrequency offset mismatch");
	static_assert(offsetof(FoliageComponent, InstanceCount) == 32, "InstanceCount offset mismatch");
	static_assert(offsetof(FoliageComponent, CastShadow) == 36, "CastShadow offset mismatch");
	static_assert(offsetof(FoliageComponent, ReceiveShadow) == 37, "ReceiveShadow offset mismatch");
	static_assert(offsetof(FoliageComponent, AlignToNormal) == 38, "AlignToNormal offset mismatch");
	static_assert(offsetof(FoliageComponent, EnableWind) == 39, "EnableWind offset mismatch");

	struct AnimGraphComponent
	{
		AssetRef AnimGraphAsset;                          // 16B (offset 0)
		float PlaybackSpeed { 1.0f };                     // 4B  (offset 16)
		float RootMotionWeight { 1.0f };                  // 4B  (offset 20)
		float FootIkWeight { 1.0f };                      // 4B  (offset 24)
		uint32_t CurrentStateHash { 0 };                  // 4B  (offset 28)
		uint8_t EnableRootMotion { 1 };                   // 1B  (offset 32)
		uint8_t EnableFootIk { 1 };                       // 1B  (offset 33)
		uint8_t EnableUpperBodyLayer { 0 };               // 1B  (offset 34)
		uint8_t _Padding[13] { 0 };                       // 13B (offset 35)

		AnimGraphComponent() = default;

		WE_SCHEMA_BODY(World, AnimGraphComponent, Component)
			WE_SCHEMA_META(Category("Animation"),
				Doc("3D animation graph state machine with blend spaces, root motion and foot IK."))
			WE_FIELD(AnimGraphAsset, Asset, Of("AnimGraph"),
				Doc("Reference to the .wanimgraph asset."));
			WE_FIELD(PlaybackSpeed, Float, Range(0.0f, 10.0f), Default(1.0f),
				Doc("Playback speed multiplier."));
			WE_FIELD(RootMotionWeight, Float, Range(0.0f, 1.0f), Default(1.0f),
				Doc("Blend weight of root motion displacement into character transform."));
			WE_FIELD(FootIkWeight, Float, Range(0.0f, 1.0f), Default(1.0f),
				Doc("Blend weight of foot ground-alignment inverse kinematics."));
			WE_FIELD(CurrentStateHash, UInt32, Default(0),
				Doc("Hash of current active state node (readback)."));
			WE_FIELD(EnableRootMotion, UInt8, Default(1),
				Doc("Whether root motion extraction is enabled."));
			WE_FIELD(EnableFootIk, UInt8, Default(1),
				Doc("Whether two-bone foot IK is enabled."));
			WE_FIELD(EnableUpperBodyLayer, UInt8, Default(0),
				Doc("Whether upper body bone masking layer is active."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(AnimGraphComponent) == 48, "AnimGraphComponent must be exactly 48 bytes");
	static_assert(std::is_trivially_copyable_v<AnimGraphComponent>, "AnimGraphComponent must be trivially copyable");
	static_assert(offsetof(AnimGraphComponent, AnimGraphAsset) == 0, "AnimGraphAsset offset mismatch");
	static_assert(offsetof(AnimGraphComponent, PlaybackSpeed) == 16, "PlaybackSpeed offset mismatch");
	static_assert(offsetof(AnimGraphComponent, RootMotionWeight) == 20, "RootMotionWeight offset mismatch");
	static_assert(offsetof(AnimGraphComponent, FootIkWeight) == 24, "FootIkWeight offset mismatch");
	static_assert(offsetof(AnimGraphComponent, CurrentStateHash) == 28, "CurrentStateHash offset mismatch");
	static_assert(offsetof(AnimGraphComponent, EnableRootMotion) == 32, "EnableRootMotion offset mismatch");
	static_assert(offsetof(AnimGraphComponent, EnableFootIk) == 33, "EnableFootIk offset mismatch");
	static_assert(offsetof(AnimGraphComponent, EnableUpperBodyLayer) == 34, "EnableUpperBodyLayer offset mismatch");

	// -----------------------------------------------------------------------
	// 通用 IK（I1）：两骨链求解（髋→膝→踝 / 肩→肘→腕 …）。
	//
	// 设计口径：
	//   * **通用系统**：TwoBoneIkComponent 只描述"哪条链、目标在哪、权重多少"，
	//     不关心是脚、手还是尾巴；同一个角色可以挂任意多条链（每条链一个目标实体）。
	//   * OwnerUuid 指向被驱动的动画实体（UUIDComponent.m_UUID）；求解发生在
	//     AnimGraph 姿态求值之后、调色板构建之前，与状态机/混合空间正交。
	//   * TargetUuid 非 0 时用该实体世界位置作为目标（可被动画/关卡脚本移动）；
	//     否则用 TargetPosition（世界坐标）。
	//   * 脚步 IK = 通用求解器之上的一个示例驱动（FootIkComponent），不是特例硬编码。
	// -----------------------------------------------------------------------
	struct TwoBoneIkComponent
	{
		uint64_t OwnerUuid { 0 };                 // 8B  (offset 0)  被驱动实体
		uint64_t TargetUuid { 0 };                // 8B  (offset 8)  可选目标实体
		NameId RootBone;                          // 4B  (offset 16) 链根（大腿/上臂）
		NameId MidBone;                           // 4B  (offset 20) 中间关节（膝/肘）
		NameId EndBone;                           // 4B  (offset 24) 末端（脚/手）
		glm::vec3 TargetPosition { 0.0f };        // 12B (offset 28) 世界坐标目标
		float Weight { 1.0f };                    // 4B  (offset 40)
		float PoleAngleDeg { 0.0f };              // 4B  (offset 44) 膝/肘朝向微调
		uint8_t Enabled { 1 };                    // 1B  (offset 48)
		uint8_t _Padding[7] { 0 };                // 7B  (offset 49)

		WE_SCHEMA_BODY(World, TwoBoneIkComponent, Component)
			WE_SCHEMA_META(Category("Animation"),
				Doc("Generic two-bone IK target (hip-knee-ankle or shoulder-elbow-wrist); foot IK is an example built on this."))
			WE_FIELD(OwnerUuid, UInt64, Default(0),
				Doc("UUID of the animated entity driven by this IK chain (0 = pick the parent entity)."));
			WE_FIELD(TargetUuid, UInt64, Default(0),
				Doc("Optional UUID of a target entity; when non-zero its world position overrides TargetPosition."));
			WE_FIELD(RootBone, Name,
				Doc("Chain root bone name (e.g. Thigh.L or UpperArm.R)."));
			WE_FIELD(MidBone, Name,
				Doc("Middle joint bone name (e.g. Shin.L or LowerArm.R)."));
			WE_FIELD(EndBone, Name,
				Doc("End effector bone name (e.g. Foot.L or Hand.R)."));
			WE_FIELD(TargetPosition, Vec3,
				Doc("World-space IK target used when TargetUuid is 0."));
			WE_FIELD(Weight, Float, Range(0.0f, 1.0f), Default(1.0f),
				Doc("Blend weight between the animated pose and the IK solution."));
			WE_FIELD(PoleAngleDeg, Float, Default(0.0f),
				Doc("Rotates the knee/elbow pole direction around the chain axis, in degrees."));
			WE_FIELD(Enabled, UInt8, Default(1),
				Doc("Whether this IK chain is solved."));
		WE_SCHEMA_END
	};

	static_assert(sizeof(TwoBoneIkComponent) == 56, "TwoBoneIkComponent must be exactly 56 bytes");
	static_assert(std::is_trivially_copyable_v<TwoBoneIkComponent>, "TwoBoneIkComponent must be trivially copyable");
	static_assert(offsetof(TwoBoneIkComponent, OwnerUuid) == 0, "OwnerUuid offset mismatch");
	static_assert(offsetof(TwoBoneIkComponent, TargetUuid) == 8, "TargetUuid offset mismatch");
	static_assert(offsetof(TwoBoneIkComponent, RootBone) == 16, "RootBone offset mismatch");
	static_assert(offsetof(TwoBoneIkComponent, MidBone) == 20, "MidBone offset mismatch");
	static_assert(offsetof(TwoBoneIkComponent, EndBone) == 24, "EndBone offset mismatch");
	static_assert(offsetof(TwoBoneIkComponent, TargetPosition) == 28, "TargetPosition offset mismatch");
	static_assert(offsetof(TwoBoneIkComponent, Weight) == 40, "Weight offset mismatch");
	static_assert(offsetof(TwoBoneIkComponent, PoleAngleDeg) == 44, "PoleAngleDeg offset mismatch");
	static_assert(offsetof(TwoBoneIkComponent, Enabled) == 48, "Enabled offset mismatch");

	// 脚步 IK 示例：通用求解器之上的"地形驱动"示例组件。
	// 它不包含任何求解数学 —— 只是把左右脚的落地目标算出来，再各发一条 TwoBoneIk 请求。
	struct FootIkComponent
	{
		float Weight { 1.0f };                    // 4B  (offset 0)
		float FootOffset { 0.0f };                // 4B  (offset 4)  脚踝离地微调（m）
		float MaxPelvisDrop { 0.35f };            // 4B  (offset 8)  示例：骨盆最大下沉（m）
		uint8_t Enabled { 1 };                    // 1B  (offset 12)
		uint8_t _Padding[3] { 0 };                // 3B  (offset 13)

		WE_SCHEMA_BODY(World, FootIkComponent, Component)
			WE_SCHEMA_META(Category("Animation"),
				Doc("Foot IK example driver built on the generic two-bone IK solver: samples terrain under each foot and issues two IK requests."));
			WE_FIELD(Weight, Float, Range(0.0f, 1.0f), Default(1.0f),
				Doc("Blend weight of both foot IK chains."));
			WE_FIELD(FootOffset, Float, Default(0.0f),
				Doc("Vertical ankle offset above the sampled ground, in meters."));
			WE_FIELD(MaxPelvisDrop, Float, Range(0.0f, 1.0f), Default(0.35f),
				Doc("Example parameter: maximum pelvis drop allowed while keeping both feet planted."));
			WE_FIELD(Enabled, UInt8, Default(1),
				Doc("Whether the foot IK example driver is active."));
		WE_SCHEMA_END
	};

	static_assert(sizeof(FootIkComponent) == 16, "FootIkComponent must be exactly 16 bytes");
	static_assert(std::is_trivially_copyable_v<FootIkComponent>, "FootIkComponent must be trivially copyable");
	static_assert(offsetof(FootIkComponent, Weight) == 0, "FootIk Weight offset mismatch");
	static_assert(offsetof(FootIkComponent, FootOffset) == 4, "FootIk FootOffset offset mismatch");
	static_assert(offsetof(FootIkComponent, MaxPelvisDrop) == 8, "FootIk MaxPelvisDrop offset mismatch");
	static_assert(offsetof(FootIkComponent, Enabled) == 12, "FootIk Enabled offset mismatch");

	struct NavMeshAgentComponent
	{
		glm::vec3 Destination { 0.0f };                   // 12B (offset 0)
		float Speed { 3.5f };                             // 4B  (offset 12)
		float Acceleration { 8.0f };                      // 4B  (offset 16)
		float StoppingDistance { 0.2f };                  // 4B  (offset 20)
		float AgentRadius { 0.5f };                       // 4B  (offset 24)
		float AgentHeight { 1.8f };                       // 4B  (offset 28: Standing physical height in meters)
		float CrouchHeight { 1.0f };                      // 4B  (offset 32: Crouching/lowered physical height in meters)
		uint32_t AreaMask { 0xFFFFFFFF };                 // 4B  (offset 36: Traversable area bitmask)
		uint8_t HasPath { 0 };                            // 1B  (offset 40)
		uint8_t ReachedDestination { 0 };                 // 1B  (offset 41)
		uint8_t AutoRepath { 1 };                         // 1B  (offset 42)
		uint8_t Crouching { 0 };                          // 1B  (offset 43: 0=Standing, 1=Crouching)
		uint8_t PathColorR { 0 };                         // 1B  (offset 44: 自定义路径颜色 R，0 = 默认预设)
		uint8_t PathColorG { 0 };                         // 1B  (offset 45: 自定义路径颜色 G)
		uint8_t PathColorB { 0 };                         // 1B  (offset 46: 自定义路径颜色 B)
		uint8_t PathColorA { 0 };                         // 1B  (offset 47: 自定义路径颜色 A，0 = 使用默认预设)

		NavMeshAgentComponent() = default;

		float GetEffectiveHeight() const { return Crouching ? CrouchHeight : AgentHeight; }

		bool HasCustomColor() const { return PathColorA > 0; }
		glm::vec4 GetCustomColor() const {
			return glm::vec4(PathColorR / 255.0f, PathColorG / 255.0f, PathColorB / 255.0f, PathColorA / 255.0f);
		}
		void SetCustomColor(const glm::vec4& col) {
			PathColorR = static_cast<uint8_t>(std::clamp(col.r * 255.0f, 0.0f, 255.0f));
			PathColorG = static_cast<uint8_t>(std::clamp(col.g * 255.0f, 0.0f, 255.0f));
			PathColorB = static_cast<uint8_t>(std::clamp(col.b * 255.0f, 0.0f, 255.0f));
			PathColorA = static_cast<uint8_t>(std::clamp(col.a * 255.0f, 1.0f, 255.0f));
		}
		void ClearCustomColor() { PathColorR = PathColorG = PathColorB = PathColorA = 0; }

		WE_SCHEMA_BODY(World, NavMeshAgentComponent, Component)
			WE_SCHEMA_META(Category("AI"),
				Doc("3D AI NavMesh navigation agent with continuous geometric clearance and capability filtering."))
			WE_FIELD(Destination, Vec3,
				Doc("Target destination in world space."));
			WE_FIELD(Speed, Float, Range(0.1f, 50.0f), Default(3.5f),
				Doc("Maximum agent traversal speed in meters/sec."));
			WE_FIELD(Acceleration, Float, Range(0.1f, 100.0f), Default(8.0f),
				Doc("Agent acceleration rate."));
			WE_FIELD(StoppingDistance, Float, Range(0.01f, 10.0f), Default(0.2f),
				Doc("Distance from target to consider arrival completed."));
			WE_FIELD(AgentRadius, Float, Range(0.1f, 5.0f), Default(0.5f),
				Doc("Cylindrical agent body radius in meters."));
			WE_FIELD(AgentHeight, Float, Range(0.1f, 10.0f), Default(1.8f),
				Doc("Standing physical body height in meters."));
			WE_FIELD(CrouchHeight, Float, Range(0.1f, 10.0f), Default(1.0f),
				Doc("Crouching/lowered body height in meters when Crouching is active."));
			WE_FIELD(Crouching, UInt8, Default(0),
				Doc("Whether character is currently in crouching/lowered posture."));
			WE_FIELD(AreaMask, UInt32, Default(0xFFFFFFFF),
				Doc("Bitmask of traversable area types (bit N corresponds to AreaType N)."));
			WE_FIELD(HasPath, UInt8, Default(0),
				Doc("Whether the agent currently has a planned path."));
			WE_FIELD(ReachedDestination, UInt8, Default(0),
				Doc("Whether the agent reached its target destination."));
			WE_FIELD(AutoRepath, UInt8, Default(1),
				Doc("Automatically recalculate path if target moves or obstacle blocks."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(NavMeshAgentComponent) == 48, "NavMeshAgentComponent must be exactly 48 bytes");
	static_assert(std::is_trivially_copyable_v<NavMeshAgentComponent>, "NavMeshAgentComponent must be trivially copyable");
	static_assert(offsetof(NavMeshAgentComponent, Destination) == 0, "Destination offset mismatch");
	static_assert(offsetof(NavMeshAgentComponent, Speed) == 12, "Speed offset mismatch");
	static_assert(offsetof(NavMeshAgentComponent, Acceleration) == 16, "Acceleration offset mismatch");
	static_assert(offsetof(NavMeshAgentComponent, StoppingDistance) == 20, "StoppingDistance offset mismatch");
	static_assert(offsetof(NavMeshAgentComponent, AgentRadius) == 24, "AgentRadius offset mismatch");
	static_assert(offsetof(NavMeshAgentComponent, AgentHeight) == 28, "AgentHeight offset mismatch");
	static_assert(offsetof(NavMeshAgentComponent, CrouchHeight) == 32, "CrouchHeight offset mismatch");
	static_assert(offsetof(NavMeshAgentComponent, AreaMask) == 36, "AreaMask offset mismatch");
	static_assert(offsetof(NavMeshAgentComponent, HasPath) == 40, "HasPath offset mismatch");
	static_assert(offsetof(NavMeshAgentComponent, ReachedDestination) == 41, "ReachedDestination offset mismatch");
	static_assert(offsetof(NavMeshAgentComponent, AutoRepath) == 42, "AutoRepath offset mismatch");
	static_assert(offsetof(NavMeshAgentComponent, Crouching) == 43, "Crouching offset mismatch");
	static_assert(offsetof(NavMeshAgentComponent, PathColorR) == 44, "PathColorR offset mismatch");
	static_assert(offsetof(NavMeshAgentComponent, PathColorA) == 47, "PathColorA offset mismatch");

	struct NavMeshObstacleComponent
	{
		glm::vec3 Extents { 1.0f, 1.0f, 1.0f };           // 12B (offset 0)
		uint8_t CarveNavMesh { 1 };                       // 1B  (offset 12)
		uint8_t IsMoving { 0 };                           // 1B  (offset 13)
		uint8_t _Padding[18] { 0 };                       // 18B (offset 14)

		NavMeshObstacleComponent() = default;

		WE_SCHEMA_BODY(World, NavMeshObstacleComponent, Component)
			WE_SCHEMA_META(Category("AI"),
				Doc("Dynamic box obstacle carving holes in 3D NavMesh."))
			WE_FIELD(Extents, Vec3,
				Doc("Half extents of the obstacle bounding box in meters."));
			WE_FIELD(CarveNavMesh, UInt8, Default(1),
				Doc("Whether to dynamically carve an impassable hole in the NavMesh."));
			WE_FIELD(IsMoving, UInt8, Default(0),
				Doc("Whether the obstacle is currently moving."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(NavMeshObstacleComponent) == 32, "NavMeshObstacleComponent must be exactly 32 bytes");
	static_assert(std::is_trivially_copyable_v<NavMeshObstacleComponent>, "NavMeshObstacleComponent must be trivially copyable");
	static_assert(offsetof(NavMeshObstacleComponent, Extents) == 0, "Extents offset mismatch");
	static_assert(offsetof(NavMeshObstacleComponent, CarveNavMesh) == 12, "CarveNavMesh offset mismatch");
	static_assert(offsetof(NavMeshObstacleComponent, IsMoving) == 13, "IsMoving offset mismatch");

	struct NavMeshOffMeshLinkComponent
	{
		glm::vec3 TargetLocation { 0.0f, 0.0f, 0.0f };   // 12B (offset 0: World target position link ends at)
		float Cost = 1.0f;                                // 4B  (offset 12: Traversal cost)
		uint8_t LinkType = 0;                             // 1B  (offset 16: 0=Manual, 1=JumpDown, 2=ClimbUp)
		uint8_t BiDirectional = 1;                        // 1B  (offset 17: Whether traversal is bidirectional)
		uint8_t _Padding[14] { 0 };                       // 14B (offset 18)

		NavMeshOffMeshLinkComponent() = default;

		WE_SCHEMA_BODY(World, NavMeshOffMeshLinkComponent, Component)
			WE_SCHEMA_META(Category("AI"),
				Doc("Off-Mesh Link connecting discontinuous NavMesh areas (e.g. jumps, drops, ladders)."))
			WE_FIELD(TargetLocation, Vec3,
				Doc("Target world position where the jump/link lands. Start position is taken from TransformComponent."));
			WE_FIELD(Cost, Float, Default(1.0f),
				Doc("Pathfinding cost multiplier across this link."));
			WE_FIELD(LinkType, UInt8, Default(0),
				Doc("Link semantic type: 0=Manual, 1=JumpDown, 2=ClimbUp."));
			WE_FIELD(BiDirectional, UInt8, Default(1),
				Doc("Whether navigation agents can traverse in both directions."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(NavMeshOffMeshLinkComponent) == 32, "NavMeshOffMeshLinkComponent must be exactly 32 bytes");
	static_assert(std::is_trivially_copyable_v<NavMeshOffMeshLinkComponent>, "NavMeshOffMeshLinkComponent must be trivially copyable");
	static_assert(offsetof(NavMeshOffMeshLinkComponent, TargetLocation) == 0, "TargetLocation offset mismatch");
	static_assert(offsetof(NavMeshOffMeshLinkComponent, Cost) == 12, "Cost offset mismatch");
	static_assert(offsetof(NavMeshOffMeshLinkComponent, LinkType) == 16, "LinkType offset mismatch");
	static_assert(offsetof(NavMeshOffMeshLinkComponent, BiDirectional) == 17, "BiDirectional offset mismatch");

	struct NavMeshAreaModifierComponent
	{
		glm::vec3 Extents { 1.0f, 1.0f, 1.0f };           // 12B (offset 0: Local half-extents)
		float CostMultiplier = 1.0f;                      // 4B  (offset 12: Cost multiplier)
		float ClearanceHeight = 0.0f;                     // 4B  (offset 16: Headroom clearance in meters; 0 = unconstrained/open air)
		uint8_t AreaType = 0;                             // 1B  (offset 20: 0=Walkable, 1=Road, 2=Mud, 3=Hazard, 4=Impassable, etc.)
		uint8_t _Padding[11] { 0 };                       // 11B (offset 21)

		NavMeshAreaModifierComponent() = default;

		WE_SCHEMA_BODY(World, NavMeshAreaModifierComponent, Component)
			WE_SCHEMA_META(Category("AI"),
				Doc("Volume modifying NavMesh area surface type, cost multiplier, and continuous headroom clearance."))
			WE_FIELD(Extents, Vec3, Default(glm::vec3(1.0f)),
				Doc("Half-extents of the area box. Multiplied by TransformComponent.Scale for world extents."));
			WE_FIELD(CostMultiplier, Float, Default(1.0f),
				Doc("Pathfinding traversal cost multiplier inside this area volume."));
			WE_FIELD(ClearanceHeight, Float, Default(0.0f),
				Doc("Continuous headroom clearance in meters (0 = unconstrained/open air). Agents taller than this cannot pass."));
			WE_FIELD(AreaType, UInt8, Default(0),
				Doc("Surface area type (0=Walkable, 1=Road, 2=Mud, 3=Hazard, 4=Impassable, or project custom)."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(NavMeshAreaModifierComponent) == 32, "NavMeshAreaModifierComponent must be exactly 32 bytes");
	static_assert(std::is_trivially_copyable_v<NavMeshAreaModifierComponent>, "NavMeshAreaModifierComponent must be trivially copyable");
	static_assert(offsetof(NavMeshAreaModifierComponent, Extents) == 0, "Extents offset mismatch");
	static_assert(offsetof(NavMeshAreaModifierComponent, CostMultiplier) == 12, "CostMultiplier offset mismatch");
	static_assert(offsetof(NavMeshAreaModifierComponent, ClearanceHeight) == 16, "ClearanceHeight offset mismatch");
	static_assert(offsetof(NavMeshAreaModifierComponent, AreaType) == 20, "AreaType offset mismatch");

	struct MeshLODComponent
	{
		AssetRef LOD1Mesh;                                // 16B (offset 0)
		AssetRef LOD2Mesh;                                // 16B (offset 16)
		float LOD1Distance { 30.0f };                     // 4B  (offset 32)
		float LOD2Distance { 80.0f };                     // 4B  (offset 36)
		float CullDistance { 180.0f };                    // 4B  (offset 40)
		float Hysteresis { 2.0f };                        // 4B  (offset 44)
		uint8_t CurrentLOD { 0 };                         // 1B  (offset 48)
		uint8_t EnableCrossFade { 1 };                    // 1B  (offset 49)
		uint8_t _Padding[14] { 0 };                       // 14B (offset 50)

		MeshLODComponent() = default;

		WE_SCHEMA_BODY(World, MeshLODComponent, Component)
			WE_SCHEMA_META(Category("Rendering"),
				Doc("Multi-level of detail (LOD) mesh switching with distance culling and hysteresis."))
			WE_FIELD(LOD1Mesh, Asset, Of("Mesh"),
				Doc("Intermediate level of detail mesh asset (LOD 1)."));
			WE_FIELD(LOD2Mesh, Asset, Of("Mesh"),
				Doc("Distant simplified level of detail mesh asset (LOD 2)."));
			WE_FIELD(LOD1Distance, Float, Range(1.0f, 1000.0f), Default(30.0f),
				Doc("Distance in meters to transition from LOD0 to LOD1."));
			WE_FIELD(LOD2Distance, Float, Range(5.0f, 2000.0f), Default(80.0f),
				Doc("Distance in meters to transition from LOD1 to LOD2."));
			WE_FIELD(CullDistance, Float, Range(10.0f, 5000.0f), Default(180.0f),
				Doc("Maximum distance beyond which all LOD meshes are culled."));
			WE_FIELD(Hysteresis, Float, Range(0.0f, 20.0f), Default(2.0f),
				Doc("Distance margin buffer to prevent rapid LOD toggling."));
			WE_FIELD(CurrentLOD, UInt8, Default(0),
				Doc("Current active LOD level index (0, 1, or 2)."));
			WE_FIELD(EnableCrossFade, UInt8, Default(1),
				Doc("Whether smooth cross-fade transition is enabled."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(MeshLODComponent) == 64, "MeshLODComponent must be exactly 64 bytes");
	static_assert(std::is_trivially_copyable_v<MeshLODComponent>, "MeshLODComponent must be trivially copyable");
	static_assert(offsetof(MeshLODComponent, LOD1Mesh) == 0, "LOD1Mesh offset mismatch");
	static_assert(offsetof(MeshLODComponent, LOD2Mesh) == 16, "LOD2Mesh offset mismatch");
	static_assert(offsetof(MeshLODComponent, LOD1Distance) == 32, "LOD1Distance offset mismatch");
	static_assert(offsetof(MeshLODComponent, LOD2Distance) == 36, "LOD2Distance offset mismatch");
	static_assert(offsetof(MeshLODComponent, CullDistance) == 40, "CullDistance offset mismatch");
	static_assert(offsetof(MeshLODComponent, Hysteresis) == 44, "Hysteresis offset mismatch");
	static_assert(offsetof(MeshLODComponent, CurrentLOD) == 48, "CurrentLOD offset mismatch");
	static_assert(offsetof(MeshLODComponent, EnableCrossFade) == 49, "EnableCrossFade offset mismatch");

	struct DecalComponent
	{
		AssetRef DecalMaterial;                           // 16B (offset 0)
		glm::vec3 Extents { 1.0f, 1.0f, 1.0f };           // 12B (offset 16)
		float FadeFactor { 1.0f };                        // 4B  (offset 28)
		float NormalAngleCutoff { 60.0f };                // 4B  (offset 32)
		uint8_t AffectTerrain { 1 };                      // 1B  (offset 36)
		uint8_t AffectMeshes { 1 };                       // 1B  (offset 37)
		uint8_t _Padding[10] { 0 };                       // 10B (offset 38)

		DecalComponent() = default;

		WE_SCHEMA_BODY(World, DecalComponent, Component)
			WE_SCHEMA_META(Category("Rendering"),
				Doc("3D projected decal box applying blood, bullet holes, footprints, or puddle details."))
			WE_FIELD(DecalMaterial, Asset, Of("Material"),
				Doc("Material or texture asset projected by this decal."));
			WE_FIELD(Extents, Vec3,
				Doc("Half size extents of the projection box in meters."));
			WE_FIELD(FadeFactor, Float, Range(0.0f, 1.0f), Default(1.0f),
				Doc("Decal opacity fade multiplier."));
			WE_FIELD(NormalAngleCutoff, Float, Range(0.0f, 89.0f), Default(60.0f),
				Doc("Maximum surface angle in degrees before decal fades out to prevent grazing stretching."));
			WE_FIELD(AffectTerrain, UInt8, Default(1),
				Doc("Whether the decal projects onto terrain."));
			WE_FIELD(AffectMeshes, UInt8, Default(1),
				Doc("Whether the decal projects onto static meshes."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(DecalComponent) == 48, "DecalComponent must be exactly 48 bytes");
	static_assert(std::is_trivially_copyable_v<DecalComponent>, "DecalComponent must be trivially copyable");
	static_assert(offsetof(DecalComponent, DecalMaterial) == 0, "DecalMaterial offset mismatch");
	static_assert(offsetof(DecalComponent, Extents) == 16, "Extents offset mismatch");
	static_assert(offsetof(DecalComponent, FadeFactor) == 28, "FadeFactor offset mismatch");
	static_assert(offsetof(DecalComponent, NormalAngleCutoff) == 32, "NormalAngleCutoff offset mismatch");
	static_assert(offsetof(DecalComponent, AffectTerrain) == 36, "AffectTerrain offset mismatch");
	static_assert(offsetof(DecalComponent, AffectMeshes) == 37, "AffectMeshes offset mismatch");

	struct ConvexCollider3DComponent
	{
		AssetRef MeshSource;                              // 16B (offset 0)
		uint32_t HullCount { 1 };                         // 4B  (offset 16)
		uint32_t MaxVerticesPerHull { 32 };               // 4B  (offset 20)
		float ConcavityThreshold { 0.05f };               // 4B  (offset 24)
		uint8_t IsTrigger { 0 };                          // 1B  (offset 28)
		uint8_t _Padding[3] { 0 };                        // 3B  (offset 29)

		ConvexCollider3DComponent() = default;

		WE_SCHEMA_BODY(World, ConvexCollider3DComponent, Component)
			WE_SCHEMA_META(Category("Physics/3D"),
				Doc("Compound convex hull collider generated via convex decomposition."))
			WE_FIELD(MeshSource, Asset, Of("Mesh"),
				Doc("Source mesh asset decomposed into convex hulls."));
			WE_FIELD(HullCount, UInt32, Default(1),
				Doc("Number of convex hull primitives in this compound collider."));
			WE_FIELD(MaxVerticesPerHull, UInt32, Range(8, 128), Default(32),
				Doc("Maximum vertex cap per convex hull."));
			WE_FIELD(ConcavityThreshold, Float, Range(0.001f, 1.0f), Default(0.05f),
				Doc("Concavity tolerance error before splitting hulls."));
			WE_FIELD(IsTrigger, UInt8, Default(0),
				Doc("Whether the collider behaves as a trigger volume."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(ConvexCollider3DComponent) == 32, "ConvexCollider3DComponent must be exactly 32 bytes");
	static_assert(std::is_trivially_copyable_v<ConvexCollider3DComponent>, "ConvexCollider3DComponent must be trivially copyable");
	static_assert(offsetof(ConvexCollider3DComponent, MeshSource) == 0, "MeshSource offset mismatch");
	static_assert(offsetof(ConvexCollider3DComponent, HullCount) == 16, "HullCount offset mismatch");
	static_assert(offsetof(ConvexCollider3DComponent, MaxVerticesPerHull) == 20, "MaxVerticesPerHull offset mismatch");
	static_assert(offsetof(ConvexCollider3DComponent, ConcavityThreshold) == 24, "ConcavityThreshold offset mismatch");
	static_assert(offsetof(ConvexCollider3DComponent, IsTrigger) == 28, "IsTrigger offset mismatch");

	struct SplineComponent
	{
		AssetRef SplineMeshAsset;                         // 16B (offset 0)
		float MeshSpacing { 2.0f };                       // 4B  (offset 16)
		float RoadWidth { 4.0f };                         // 4B  (offset 20)
		uint8_t ClosedLoop { 0 };                         // 1B  (offset 24)
		uint8_t GenerateCollision { 1 };                  // 1B  (offset 25)
		uint8_t ExtrudeRoad { 1 };                        // 1B  (offset 26)
		uint8_t _Padding[21] { 0 };                       // 21B (offset 27)

		SplineComponent() = default;

		WE_SCHEMA_BODY(World, SplineComponent, Component)
			WE_SCHEMA_META(Category("Rendering"),
				Doc("3D parametric spline path for roads, rivers, fences and patrol rails."))
			WE_FIELD(SplineMeshAsset, Asset, Of("Mesh"),
				Doc("Mesh repeated or extruded along the spline curve."));
			WE_FIELD(MeshSpacing, Float, Range(0.1f, 100.0f), Default(2.0f),
				Doc("Distance spacing between extruded mesh segments."));
			WE_FIELD(RoadWidth, Float, Range(0.1f, 50.0f), Default(4.0f),
				Doc("Width of the procedural extruded road surface."));
			WE_FIELD(ClosedLoop, UInt8, Default(0),
				Doc("Whether the spline connects back into a closed loop."));
			WE_FIELD(GenerateCollision, UInt8, Default(1),
				Doc("Whether to generate collision for the extruded surface."));
			WE_FIELD(ExtrudeRoad, UInt8, Default(1),
				Doc("Whether to automatically generate an extruded ribbon mesh."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(SplineComponent) == 48, "SplineComponent must be exactly 48 bytes");
	static_assert(std::is_trivially_copyable_v<SplineComponent>, "SplineComponent must be trivially copyable");
	static_assert(offsetof(SplineComponent, SplineMeshAsset) == 0, "SplineMeshAsset offset mismatch");
	static_assert(offsetof(SplineComponent, MeshSpacing) == 16, "MeshSpacing offset mismatch");
	static_assert(offsetof(SplineComponent, RoadWidth) == 20, "RoadWidth offset mismatch");
	static_assert(offsetof(SplineComponent, ClosedLoop) == 24, "ClosedLoop offset mismatch");
	static_assert(offsetof(SplineComponent, GenerateCollision) == 25, "GenerateCollision offset mismatch");
	static_assert(offsetof(SplineComponent, ExtrudeRoad) == 26, "ExtrudeRoad offset mismatch");

	struct WhiteboxComponent
	{
		enum class ShapeType : uint8_t { Box = 0, Cylinder = 1, Stairs = 2, Wedge = 3, Arch = 4 };
		ShapeType Shape { ShapeType::Box };               // 1B  (offset 0)
		uint8_t Subdivisions { 1 };                       // 1B  (offset 1)
		uint8_t Steps { 8 };                              // 1B  (offset 2)
		uint8_t _Padding0 { 0 };                          // 1B  (offset 3)
		glm::vec3 Dimensions { 2.0f, 2.0f, 2.0f };        // 12B (offset 4)
		float BevelSize { 0.0f };                         // 4B  (offset 16)
		uint8_t GenerateCollision { 1 };                  // 1B  (offset 20)
		uint8_t _Padding[11] { 0 };                       // 11B (offset 21)

		WhiteboxComponent() = default;

		WE_SCHEMA_BODY(World, WhiteboxComponent, Component)
			WE_SCHEMA_META(Category("Rendering"),
				Doc("Procedural geometric prototyping brush (Box, Cylinder, Stairs, Wedge, Arch)."))
			WE_FIELD(Dimensions, Vec3,
				Doc("Prototyping brush dimensions in meters (X, Y, Z)."));
			WE_FIELD(BevelSize, Float, Range(0.0f, 1.0f), Default(0.0f),
				Doc("Bevel chamfer size applied to polygon edges."));
			WE_FIELD(Steps, UInt8, Range(1, 64), Default(8),
				Doc("Number of stair steps (for Stairs shape)."));
			WE_FIELD(GenerateCollision, UInt8, Default(1),
				Doc("Whether to generate static collision for the prototype."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(WhiteboxComponent) == 32, "WhiteboxComponent must be exactly 32 bytes");
	static_assert(std::is_trivially_copyable_v<WhiteboxComponent>, "WhiteboxComponent must be trivially copyable");
	static_assert(offsetof(WhiteboxComponent, Shape) == 0, "Shape offset mismatch");
	static_assert(offsetof(WhiteboxComponent, Subdivisions) == 1, "Subdivisions offset mismatch");
	static_assert(offsetof(WhiteboxComponent, Steps) == 2, "Steps offset mismatch");
	static_assert(offsetof(WhiteboxComponent, Dimensions) == 4, "Dimensions offset mismatch");
	static_assert(offsetof(WhiteboxComponent, BevelSize) == 16, "BevelSize offset mismatch");
	static_assert(offsetof(WhiteboxComponent, GenerateCollision) == 20, "GenerateCollision offset mismatch");

	struct HLODComponent
	{
		AssetRef ProxyMesh;                               // 16B (offset 0)
		float TransitionDistance { 200.0f };              // 4B  (offset 16)
		float CullDistance { 1000.0f };                   // 4B  (offset 20)
		uint32_t ChildEntityCount { 0 };                  // 4B  (offset 24)
		uint8_t IsActive { 0 };                           // 1B  (offset 28)
		uint8_t CastShadow { 1 };                         // 1B  (offset 29)
		uint8_t _Padding[18] { 0 };                       // 18B (offset 30)

		HLODComponent() = default;

		WE_SCHEMA_BODY(World, HLODComponent, Component)
			WE_SCHEMA_META(Category("Rendering"),
				Doc("Hierarchical Level of Detail proxy cluster aggregating distant scene entities."))
			WE_FIELD(ProxyMesh, Asset, Of("Mesh"),
				Doc("Combined distant proxy mesh asset."));
			WE_FIELD(TransitionDistance, Float, Range(50.0f, 5000.0f), Default(200.0f),
				Doc("Distance in meters where individual entities are hidden and proxy is rendered."));
			WE_FIELD(CullDistance, Float, Range(100.0f, 20000.0f), Default(1000.0f),
				Doc("Maximum draw distance of the HLOD proxy."));
			WE_FIELD(IsActive, UInt8, Default(0),
				Doc("Whether the HLOD proxy is currently being drawn."));
			WE_FIELD(CastShadow, UInt8, Default(1),
				Doc("Whether the proxy casts shadows."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(HLODComponent) == 48, "HLODComponent must be exactly 48 bytes");
	static_assert(std::is_trivially_copyable_v<HLODComponent>, "HLODComponent must be trivially copyable");
	static_assert(offsetof(HLODComponent, ProxyMesh) == 0, "ProxyMesh offset mismatch");
	static_assert(offsetof(HLODComponent, TransitionDistance) == 16, "TransitionDistance offset mismatch");
	static_assert(offsetof(HLODComponent, CullDistance) == 20, "CullDistance offset mismatch");
	static_assert(offsetof(HLODComponent, ChildEntityCount) == 24, "ChildEntityCount offset mismatch");
	static_assert(offsetof(HLODComponent, IsActive) == 28, "IsActive offset mismatch");
	static_assert(offsetof(HLODComponent, CastShadow) == 29, "CastShadow offset mismatch");

	struct RVTComponent
	{
		AssetRef VirtualTexture;                          // 16B (offset 0)
		float SoftBlendRadius { 0.5f };                   // 4B  (offset 16)
		uint8_t WriteAlbedo { 1 };                        // 1B  (offset 20)
		uint8_t WriteNormal { 1 };                        // 1B  (offset 21)
		uint8_t WriteRoughness { 1 };                     // 1B  (offset 22)
		uint8_t _Padding[9] { 0 };                        // 9B  (offset 23)

		RVTComponent() = default;

		WE_SCHEMA_BODY(World, RVTComponent, Component)
			WE_SCHEMA_META(Category("Rendering"),
				Doc("Runtime Virtual Texture component for ground caching and soft contact blending."))
			WE_FIELD(VirtualTexture, Asset, Of("Texture"),
				Doc("Target virtual texture container asset."));
			WE_FIELD(SoftBlendRadius, Float, Range(0.01f, 10.0f), Default(0.5f),
				Doc("Edge contact soft blend transition width in meters."));
			WE_FIELD(WriteAlbedo, UInt8, Default(1),
				Doc("Whether to write base color to virtual texture."));
			WE_FIELD(WriteNormal, UInt8, Default(1),
				Doc("Whether to write surface normal to virtual texture."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(RVTComponent) == 32, "RVTComponent must be exactly 32 bytes");
	static_assert(std::is_trivially_copyable_v<RVTComponent>, "RVTComponent must be trivially copyable");
	static_assert(offsetof(RVTComponent, VirtualTexture) == 0, "VirtualTexture offset mismatch");
	static_assert(offsetof(RVTComponent, SoftBlendRadius) == 16, "SoftBlendRadius offset mismatch");
	static_assert(offsetof(RVTComponent, WriteAlbedo) == 20, "WriteAlbedo offset mismatch");
	static_assert(offsetof(RVTComponent, WriteNormal) == 21, "WriteNormal offset mismatch");

	struct WaterBodyComponent
	{
		enum class WaterType : uint8_t { Ocean = 0, Lake = 1, River = 2 };
		WaterType Type { WaterType::Ocean };              // 1B  (offset 0)
		uint8_t EnableFoam { 1 };                         // 1B  (offset 1)
		uint8_t EnableCaustics { 1 };                     // 1B  (offset 2)
		uint8_t EnableBuoyancy { 1 };                     // 1B  (offset 3)
		float WaveSteepness { 0.5f };                     // 4B  (offset 4)
		float WaveLength { 20.0f };                       // 4B  (offset 8)
		float WaveSpeed { 2.0f };                         // 4B  (offset 12)
		float WaterClarity { 15.0f };                     // 4B  (offset 16)
		glm::vec3 WaterColor { 0.05f, 0.25f, 0.35f };     // 12B (offset 20)
		uint8_t _Padding[16] { 0 };                       // 16B (offset 32..47)

		WaterBodyComponent() = default;

		WE_SCHEMA_BODY(World, WaterBodyComponent, Component)
			WE_SCHEMA_META(Category("Environment"),
				Doc("Dynamic physical ocean, lake or river water body with Gerstner waves and buoyancy."))
			WE_FIELD(WaveSteepness, Float, Range(0.0f, 1.0f), Default(0.5f),
				Doc("Steepness sharpness of the Gerstner waves."));
			WE_FIELD(WaveLength, Float, Range(1.0f, 500.0f), Default(20.0f),
				Doc("Primary wave wavelength in meters."));
			WE_FIELD(WaveSpeed, Float, Range(0.0f, 20.0f), Default(2.0f),
				Doc("Wave propagation speed in meters/second."));
			WE_FIELD(WaterClarity, Float, Range(0.1f, 100.0f), Default(15.0f),
				Doc("Depth attenuation distance in meters for optical water absorption."));
			WE_FIELD(WaterColor, Vec3, Color(),
				Doc("Deep water absorption tint color."));
			WE_FIELD(EnableFoam, UInt8, Default(1),
				Doc("Enable shoreline foam and wave-tip crest foam."));
			WE_FIELD(EnableBuoyancy, UInt8, Default(1),
				Doc("Enable physics buoyancy query integration."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(WaterBodyComponent) == 48, "WaterBodyComponent must be exactly 48 bytes");
	static_assert(std::is_trivially_copyable_v<WaterBodyComponent>, "WaterBodyComponent must be trivially copyable");
	static_assert(offsetof(WaterBodyComponent, Type) == 0, "Type offset mismatch");
	static_assert(offsetof(WaterBodyComponent, EnableFoam) == 1, "EnableFoam offset mismatch");
	static_assert(offsetof(WaterBodyComponent, EnableCaustics) == 2, "EnableCaustics offset mismatch");
	static_assert(offsetof(WaterBodyComponent, EnableBuoyancy) == 3, "EnableBuoyancy offset mismatch");
	static_assert(offsetof(WaterBodyComponent, WaveSteepness) == 4, "WaveSteepness offset mismatch");
	static_assert(offsetof(WaterBodyComponent, WaveLength) == 8, "WaveLength offset mismatch");
	static_assert(offsetof(WaterBodyComponent, WaveSpeed) == 12, "WaveSpeed offset mismatch");
	static_assert(offsetof(WaterBodyComponent, WaterClarity) == 16, "WaterClarity offset mismatch");
	static_assert(offsetof(WaterBodyComponent, WaterColor) == 20, "WaterColor offset mismatch");

	struct WeatherSystemComponent
	{
		enum class WeatherType : uint8_t { Clear = 0, Overcast = 1, Rain = 2, Storm = 3, Snow = 4 };
		WeatherType CurrentWeather { WeatherType::Clear }; // 1B  (offset 0)
		uint8_t EnableVolumetricCloud { 1 };               // 1B  (offset 1)
		uint8_t EnablePuddles { 1 };                       // 1B  (offset 2)
		uint8_t _Padding0 { 0 };                           // 1B  (offset 3)
		float PrecipitationIntensity { 0.0f };             // 4B  (offset 4)
		float CloudCoverage { 0.3f };                      // 4B  (offset 8)
		float CloudDensity { 1.0f };                       // 4B  (offset 12)
		float Wetness { 0.0f };                            // 4B  (offset 16)
		float WindSpeed { 5.0f };                          // 4B  (offset 20)
		glm::vec3 WindDirection { 1.0f, 0.0f, 0.0f };     // 12B (offset 24)
		uint8_t _Padding[12] { 0 };                        // 12B (offset 36..47)

		WeatherSystemComponent() = default;

		WE_SCHEMA_BODY(World, WeatherSystemComponent, Component)
			WE_SCHEMA_META(Category("Environment"),
				Doc("Dynamic atmospheric weather, volumetric clouds, precipitation and ground wetness."))
			WE_FIELD(PrecipitationIntensity, Float, Range(0.0f, 1.0f), Default(0.0f),
				Doc("Rain/snow precipitation intensity [0, 1]."));
			WE_FIELD(CloudCoverage, Float, Range(0.0f, 1.0f), Default(0.3f),
				Doc("Sky coverage ratio of volumetric clouds."));
			WE_FIELD(CloudDensity, Float, Range(0.1f, 10.0f), Default(1.0f),
				Doc("Optical density of raymarched clouds."));
			WE_FIELD(Wetness, Float, Range(0.0f, 1.0f), Default(0.0f),
				Doc("Accumulated ground wetness and puddle level [0, 1]."));
			WE_FIELD(WindSpeed, Float, Range(0.0f, 50.0f), Default(5.0f),
				Doc("Atmospheric wind velocity in meters/second."));
			WE_FIELD(WindDirection, Vec3,
				Doc("Global wind direction vector."));
			WE_FIELD(EnableVolumetricCloud, UInt8, Default(1),
				Doc("Enable raymarched volumetric cloud rendering."));
			WE_FIELD(EnablePuddles, UInt8, Default(1),
				Doc("Enable ground wetness and procedural puddle accumulation."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(WeatherSystemComponent) == 48, "WeatherSystemComponent must be exactly 48 bytes");
	static_assert(std::is_trivially_copyable_v<WeatherSystemComponent>, "WeatherSystemComponent must be trivially copyable");
	static_assert(offsetof(WeatherSystemComponent, CurrentWeather) == 0, "CurrentWeather offset mismatch");
	static_assert(offsetof(WeatherSystemComponent, EnableVolumetricCloud) == 1, "EnableVolumetricCloud offset mismatch");
	static_assert(offsetof(WeatherSystemComponent, EnablePuddles) == 2, "EnablePuddles offset mismatch");
	static_assert(offsetof(WeatherSystemComponent, PrecipitationIntensity) == 4, "PrecipitationIntensity offset mismatch");
	static_assert(offsetof(WeatherSystemComponent, CloudCoverage) == 8, "CloudCoverage offset mismatch");
	static_assert(offsetof(WeatherSystemComponent, CloudDensity) == 12, "CloudDensity offset mismatch");
	static_assert(offsetof(WeatherSystemComponent, Wetness) == 16, "Wetness offset mismatch");
	static_assert(offsetof(WeatherSystemComponent, WindSpeed) == 20, "WindSpeed offset mismatch");
	static_assert(offsetof(WeatherSystemComponent, WindDirection) == 24, "WindDirection offset mismatch");

	struct BiomePCGComponent
	{
		AssetRef RuleAsset;                               // 16B (offset 0)
		float MinElevation { 0.0f };                      // 4B  (offset 16)
		float MaxElevation { 1000.0f };                   // 4B  (offset 20)
		float MaxSlope { 35.0f };                         // 4B  (offset 24)
		float MoistureThreshold { 0.3f };                 // 4B  (offset 28)
		uint32_t Seed { 1337 };                           // 4B  (offset 32)
		uint8_t AutoDeformTerrain { 1 };                  // 1B  (offset 36)
		uint8_t _Padding[11] { 0 };                       // 11B (offset 37..47)

		BiomePCGComponent() = default;

		WE_SCHEMA_BODY(World, BiomePCGComponent, Component)
			WE_SCHEMA_META(Category("Environment"),
				Doc("Procedural Content Generation rules for biomes, vegetation clusters and terrain deformation."))
			WE_FIELD(RuleAsset, Asset, Of("PCGRule"),
				Doc("Rule graph definition asset."));
			WE_FIELD(MinElevation, Float, Range(0.0f, 5000.0f), Default(0.0f),
				Doc("Minimum elevation bound in meters."));
			WE_FIELD(MaxElevation, Float, Range(0.0f, 5000.0f), Default(1000.0f),
				Doc("Maximum elevation bound in meters."));
			WE_FIELD(MaxSlope, Float, Range(0.0f, 89.0f), Default(35.0f),
				Doc("Maximum slope angle in degrees where foliage or settlements can spawn."));
			WE_FIELD(MoistureThreshold, Float, Range(0.0f, 1.0f), Default(0.3f),
				Doc("Minimum moisture humidity threshold."));
			WE_FIELD(Seed, UInt32, Default(1337),
				Doc("Procedural pseudorandom seed."));
			WE_FIELD(AutoDeformTerrain, UInt8, Default(1),
				Doc("Automatically cut, fill, and slope terrain along spline roads."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(BiomePCGComponent) == 48, "BiomePCGComponent must be exactly 48 bytes");
	static_assert(std::is_trivially_copyable_v<BiomePCGComponent>, "BiomePCGComponent must be trivially copyable");
	static_assert(offsetof(BiomePCGComponent, RuleAsset) == 0, "RuleAsset offset mismatch");
	static_assert(offsetof(BiomePCGComponent, MinElevation) == 16, "MinElevation offset mismatch");
	static_assert(offsetof(BiomePCGComponent, MaxElevation) == 20, "MaxElevation offset mismatch");
	static_assert(offsetof(BiomePCGComponent, MaxSlope) == 24, "MaxSlope offset mismatch");
	static_assert(offsetof(BiomePCGComponent, MoistureThreshold) == 28, "MoistureThreshold offset mismatch");
	static_assert(offsetof(BiomePCGComponent, Seed) == 32, "Seed offset mismatch");
	static_assert(offsetof(BiomePCGComponent, AutoDeformTerrain) == 36, "AutoDeformTerrain offset mismatch");

	struct EQSComponent
	{
		glm::vec3 QueryCenter { 0.0f };                   // 12B (offset 0)
		float QueryRadius { 20.0f };                      // 4B  (offset 12)
		float GridSpacing { 2.0f };                       // 4B  (offset 16)
		uint8_t BestCoverFound { 0 };                     // 1B  (offset 20)
		uint8_t _Padding[11] { 0 };                       // 11B (offset 21..31)

		EQSComponent() = default;

		WE_SCHEMA_BODY(World, EQSComponent, Component)
			WE_SCHEMA_META(Category("AI"),
				Doc("Environment Query System for finding tactical cover points and line-of-sight concealment."))
			WE_FIELD(QueryCenter, Vec3,
				Doc("Center coordinates of the tactical query in world space."));
			WE_FIELD(QueryRadius, Float, Range(1.0f, 200.0f), Default(20.0f),
				Doc("Query radius in meters."));
			WE_FIELD(GridSpacing, Float, Range(0.5f, 20.0f), Default(2.0f),
				Doc("Spacing distance between candidate sample points in meters."));
			WE_FIELD(BestCoverFound, UInt8, Default(0),
				Doc("Whether an optimal tactical cover point was found."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(EQSComponent) == 32, "EQSComponent must be exactly 32 bytes");
	static_assert(std::is_trivially_copyable_v<EQSComponent>, "EQSComponent must be trivially copyable");
	static_assert(offsetof(EQSComponent, QueryCenter) == 0, "QueryCenter offset mismatch");
	static_assert(offsetof(EQSComponent, QueryRadius) == 12, "QueryRadius offset mismatch");
	static_assert(offsetof(EQSComponent, GridSpacing) == 16, "GridSpacing offset mismatch");
	static_assert(offsetof(EQSComponent, BestCoverFound) == 20, "BestCoverFound offset mismatch");

	struct FoliageInteractionComponent
	{
		float PushRadius { 1.5f };                        // 4B  (offset 0)
		float PushStrength { 1.0f };                      // 4B  (offset 4)
		float RecoverySpeed { 3.0f };                     // 4B  (offset 8)
		uint8_t LeaveTracks { 1 };                        // 1B  (offset 12)
		uint8_t _Padding[19] { 0 };                       // 19B (offset 13..31)

		FoliageInteractionComponent() = default;

		WE_SCHEMA_BODY(World, FoliageInteractionComponent, Component)
			WE_SCHEMA_META(Category("Environment"),
				Doc("Physical foliage bending and track deformation when entities traverse vegetation."))
			WE_FIELD(PushRadius, Float, Range(0.1f, 20.0f), Default(1.5f),
				Doc("Radius around the entity where foliage is pushed and bent."));
			WE_FIELD(PushStrength, Float, Range(0.0f, 5.0f), Default(1.0f),
				Doc("Force displacement strength of the foliage bend."));
			WE_FIELD(RecoverySpeed, Float, Range(0.1f, 20.0f), Default(3.0f),
				Doc("Elastic spring recovery rate returning foliage upright."));
			WE_FIELD(LeaveTracks, UInt8, Default(1),
				Doc("Whether traversing leaves flattened tracks behind the entity."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(FoliageInteractionComponent) == 32, "FoliageInteractionComponent must be exactly 32 bytes");
	static_assert(std::is_trivially_copyable_v<FoliageInteractionComponent>, "FoliageInteractionComponent must be trivially copyable");
	static_assert(offsetof(FoliageInteractionComponent, PushRadius) == 0, "PushRadius offset mismatch");
	static_assert(offsetof(FoliageInteractionComponent, PushStrength) == 4, "PushStrength offset mismatch");
	static_assert(offsetof(FoliageInteractionComponent, RecoverySpeed) == 8, "RecoverySpeed offset mismatch");
	static_assert(offsetof(FoliageInteractionComponent, LeaveTracks) == 12, "LeaveTracks offset mismatch");

	enum class LocalWindType : uint8_t
	{
		Directional = 0,
		Radial = 1,
		Vortex = 2
	};
	WE_ENUM_SCHEMA(World, LocalWindType, UInt8)
		WE_ENUM_VALUE(Directional);
		WE_ENUM_VALUE(Radial);
		WE_ENUM_VALUE(Vortex);
	WE_ENUM_END

	struct LocalWindComponent
	{
		glm::vec3 Direction { 1.0f, 0.0f, 0.0f };         // 12B (offset 0..11)
		float Radius { 10.0f };                           // 4B  (offset 12..15)
		float Strength { 2.5f };                          // 4B  (offset 16..19)
		float Speed { 3.0f };                             // 4B  (offset 20..23)
		float Decay { 1.5f };                             // 4B  (offset 24..27)
		LocalWindType Type { LocalWindType::Radial };     // 1B  (offset 28)
		uint8_t ImpulseActive { 0 };                      // 1B  (offset 29)
		uint8_t _Padding[2] { 0 };                        // 2B  (offset 30..31)

		LocalWindComponent() = default;

		WE_SCHEMA_BODY(World, LocalWindComponent, Component)
			WE_SCHEMA_META(Category("Environment"),
				Doc("Local dynamic wind source affecting foliage and vegetation (explosions, vortices, wind tunnels)."));
			WE_FIELD(Direction, Vec3,
				Doc("Primary wind direction or vortex rotation axis."));
			WE_FIELD(Radius, Float, Range(0.5f, 100.0f), Default(10.0f),
				Doc("Effective spherical/cylindrical radius in meters."));
			WE_FIELD(Strength, Float, Range(0.0f, 20.0f), Default(2.5f),
				Doc("Peak wind displacement force."));
			WE_FIELD(Speed, Float, Range(0.1f, 30.0f), Default(3.0f),
				Doc("Oscillation wave frequency or vortex angular speed."));
			WE_FIELD(Decay, Float, Range(0.5f, 4.0f), Default(1.5f),
				Doc("Distance attenuation falloff exponent."));
			WE_FIELD(Type, Enum, Of(LocalWindType),
				Doc("Wind pattern: Directional (wind tunnel), Radial (blast/downwash), Vortex (swirl)."));
			WE_FIELD(ImpulseActive, UInt8, Default(0),
				Doc("Whether this is a decaying transient impulse (e.g. shockwave)."));
		WE_SCHEMA_END
	};
	static_assert(sizeof(LocalWindComponent) == 32, "LocalWindComponent must be exactly 32 bytes");
	static_assert(std::is_trivially_copyable_v<LocalWindComponent>, "LocalWindComponent must be trivially copyable");
	static_assert(offsetof(LocalWindComponent, Direction) == 0, "Direction offset mismatch");
	static_assert(offsetof(LocalWindComponent, Radius) == 12, "Radius offset mismatch");
	static_assert(offsetof(LocalWindComponent, Strength) == 16, "Strength offset mismatch");
	static_assert(offsetof(LocalWindComponent, Speed) == 20, "Speed offset mismatch");
	static_assert(offsetof(LocalWindComponent, Decay) == 24, "Decay offset mismatch");
	static_assert(offsetof(LocalWindComponent, Type) == 28, "Type offset mismatch");
	static_assert(offsetof(LocalWindComponent, ImpulseActive) == 29, "ImpulseActive offset mismatch");

}


