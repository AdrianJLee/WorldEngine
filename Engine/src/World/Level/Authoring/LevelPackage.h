#pragma once

#include "World/Core/Core.h"
#include "World/Core/Export.h"
#include "World/Level/Authoring/EntityDescriptor.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace World
{
	class Scene;
	class WorldContext;
}

namespace World::Level
{
	struct DataLayerDefinition
	{
		uint32_t LayerMask{ 1 };
		std::string Name;
		bool DefaultActive{ true };
	};

	// ---- 数据层定义的规则与工具(引擎侧唯一实现;面板/导入/保存共用)----
	//
	// 为什么放在引擎而不是面板里:层定义会**落盘进 manifest**,而 manifest 是外部可读可手改的
	// 事实源。规则(每层恰一位、位不重复、排序确定、至少一层)如果只活在面板里,手改 manifest
	// 或旧文件就会让"视口里出现两个同掩码的开关"这类问题重新长出来。
	//
	// **归一化**规则:
	//   * 丢弃 `LayerMask == 0` 与**非单一位**的定义(一层恰一位,多位掩码是实体侧的用法);
	//   * 位重复保留首个(丢弃后来的);
	//   * 空名字按位补默认名 `Layer<n>`(n = 位序,1 起);
	//   * 按掩码升序排序(顺序稳定 ⇒ manifest 逐字节确定性,便于 diff 与门禁);
	//   * 全部无效 ⇒ 回填常驻层 `{ 1, "Base_World", true }`(关卡必须至少有一个可归属的层)。
	// 返回 true = 内容有改动(调用方据此决定是否重写 manifest)。
	WLD_API bool NormalizeDataLayers(std::vector<DataLayerDefinition>& layers);

	// 下一个空闲的**单个**位(自 bit0 起);32 位全部占用 ⇒ 0(调用方据此禁用"添加层")。
	WLD_API uint32_t NextFreeLayerMask(const std::vector<DataLayerDefinition>& layers);

	// 该掩码是否**完全**被层定义覆盖(0 = 常驻可见,恒为 true)。
	// 用途:删除层的守卫 —— 有实体用到未被覆盖的位时拒绝删除,避免静默改变可见性语义。
	WLD_API bool IsMaskCoveredByLayers(uint32_t mask, const std::vector<DataLayerDefinition>& layers);

	// 关卡级世界设置(`.wlevel` 的 `World:` 块)。
	//
	// 为什么在 manifest 而不是实体级:重力覆盖、物理调试是**整个世界**的属性,
	// 不挂在任何单个实体上,所以进不了 `.entity`(那是一个实体的文件)。
	// 它也不属于预制体 —— 预制体是可复用子树,不带世界设置。
	enum class PartitionGridPlane : uint8_t
	{
		HorizontalXZ = 0, // 3D 水平大世界地平面 [默认]
		VerticalXY = 1,   // 2D 正面立面 (横版跳跃/卷轴)
		VerticalZY = 2,   // 2D 侧面立面 (纵向进深/剖面)
		Isometric = 3,    // 2.5D 等轴测斜向 (45°菱形网格, 如暗黑/哈迪斯)
		Full3D = 4,       // 纯 3D 立体分块 (立方体体素, 如太空/天坑)
	};

	struct LevelPartitionSettings
	{
		uint32_t GridCellSize{ 64 };
		PartitionGridPlane GridPlane{ PartitionGridPlane::HorizontalXZ };
		float StreamingRadius{ 128.0f };

		bool IsDefault() const
		{
			return GridCellSize == 64 &&
			       GridPlane == PartitionGridPlane::HorizontalXZ &&
			       std::abs(StreamingRadius - 128.0f) < 1e-3f;
		}
	};

	struct LevelWorldSettings
	{
		bool HasGravityOverride{ false };
		float Gravity{ -9.81f };
		bool PhysicsDebug{ false };

		bool IsDefault() const { return !HasGravityOverride && !PhysicsDebug; }
	};

	// 关卡包 = **入口文件**(`scenes/<Id>.wlevel`,存 manifest)+ **内容目录**(`levels/<Id>/`)。
	//
	// 入口与内容分离是刻意的:入口是"能被打开的那个文件"(内容浏览器可双击、启动场景可指向它),
	// 内容按 OFPA 拆成一实体一文件(多人协作零冲突)。二者按 **Id 同名**成对,由
	// `LevelCatalog` 校验 —— 不一致即报错,不静默降级。
	//
	// 入口路径是**唯一**入参:`ContentDir()` 由它推导,不存在"同名耦合"的第二个事实源。
	class WLD_API LevelPackage
	{
	public:
		static constexpr const char* EntryExtension = ".wlevel";
		static constexpr const char* LevelsDirName = "levels";
		static constexpr const char* ScenesDirName = "scenes";

		LevelPackage();
		~LevelPackage();

		LevelPackage(LevelPackage&& other) noexcept;
		LevelPackage& operator=(LevelPackage&& other) noexcept;
		LevelPackage(const LevelPackage&) = delete;
		LevelPackage& operator=(const LevelPackage&) = delete;

		const std::string& GetId() const;
		void SetId(const std::string& id);

		const std::string& GetName() const;
		void SetName(const std::string& name);

		const LevelPartitionSettings& GetPartitionSettings() const;
		void SetPartitionSettings(const LevelPartitionSettings& settings);

		uint32_t GetGridCellSize() const;
		void SetGridCellSize(uint32_t size);

		const LevelWorldSettings& GetWorld() const;
		LevelWorldSettings& GetWorld();

		const std::vector<DataLayerDefinition>& GetDataLayers() const;
		std::vector<DataLayerDefinition>& GetDataLayers();

		size_t GetEntityCount() const;
		const EntityDescriptor& GetEntity(size_t index) const;
		EntityDescriptor& GetMutableEntity(size_t index);
		void AddEntity(const EntityDescriptor& entity);
		const EntityDescriptor* FindEntity(uint64_t entityUuid) const;

		// 入口文件路径(`scenes/<Id>.wlevel`)。未加载/未设置时为空。
		const std::filesystem::path& GetEntryPath() const { return m_EntryPath; }
		// 预先设置入口路径(内容根由它推导)。捕获侧(CaptureFromScene)需要它来解析实例
		// 引用的 `.wprefab`,因此 `Save` **之前**就要设好 —— 编辑器保存即走这条。
		void SetEntryPath(const std::filesystem::path& entryPath) { m_EntryPath = entryPath; }
		// 内容目录:入口路径推导 —— `<内容根>/levels/<Id>`。入口未设置时返回空。
		std::filesystem::path ContentDir() const;

		// 写入口 manifest + 内容目录的 `entities/*.entity`(并剪掉已删除实体的旧文件)。
		bool Save(const std::filesystem::path& entryPath, std::string* outError = nullptr);
		// 仅写入口 manifest(内容不变时用;`Save` 已包含它)。
		bool SaveEntry(std::string* outError = nullptr) const;
		bool Load(const std::filesystem::path& entryPath, std::string* outError = nullptr);

		// 新建一个可用的空关卡:写入口 `.wlevel` + `levels/<Id>/entities/` 起手实体
		// (相机 / 方向光 / 立方体),走同一个"场景 → 包"捕获路径 ⇒ 带全部组件。
		// 入口已存在、或内容目录已存在且非空 ⇒ 拒绝(不覆盖别人的关卡)。
		static bool CreateEmpty(WorldContext& context, const std::filesystem::path& entryPath,
			const std::string& id, std::string* error = nullptr);

		// 由场景重建整包:逐实体走 SceneSerializer 的**实体级**序列化 —— 全部组件随文件走,
		// 数据层归属来自实体自身的 LevelEntityComponent。已有实体集合会被清空重建。
		void CaptureFromScene(Scene& scene, const std::string& levelName, uint32_t gridCellSize);

		// 把整包实体并入一个新场景。逐实体走 SceneSerializer 的实体级路径(全部组件),
		// 末尾 FinalizeEntityImport 统一重算 Transform 缓存矩阵与层级反向索引。
		Ref<Scene> Instantiate(WorldContext& context) const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
		std::filesystem::path m_EntryPath;
	};
}
