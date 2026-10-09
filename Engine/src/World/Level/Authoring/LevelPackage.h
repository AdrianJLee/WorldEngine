#pragma once

#include "World/Core/Core.h"
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

	class LevelPackage
	{
	public:
		LevelPackage();
		~LevelPackage();

		LevelPackage(LevelPackage&& other) noexcept;
		LevelPackage& operator=(LevelPackage&& other) noexcept;

		LevelPackage(const LevelPackage&) = delete;
		LevelPackage& operator=(const LevelPackage&) = delete;

		const std::string& GetName() const;
		void SetName(const std::string& name);

		uint32_t GetGridCellSize() const;
		void SetGridCellSize(uint32_t size);

		const std::vector<DataLayerDefinition>& GetDataLayers() const;
		std::vector<DataLayerDefinition>& GetDataLayers();

		size_t GetEntityCount() const;
		const EntityDescriptor& GetEntity(size_t index) const;
		// 可变访问:烘焙前调整 ChunkID / DataLayerMask 等关卡层元数据(工具链用)。
		EntityDescriptor& GetMutableEntity(size_t index);
		void AddEntity(const EntityDescriptor& entity);
		const EntityDescriptor* FindEntity(uint64_t entityUuid) const;

		bool Save(const std::filesystem::path& packageDir, std::string* outError = nullptr) const;
		bool Load(const std::filesystem::path& packageDir, std::string* outError = nullptr);

		// 新建一个可用的空关卡:`manifest.yaml`(Base_World 层)+ 起手实体
		// (相机 / 方向光 / 一个立方体)。起手实体走同一个场景 → 包 的捕获路径,
		// 因此它们与编辑器里手摆的实体逐字段同形(带全部组件)。
		//
		// 目标目录已存在且非空 ⇒ 拒绝(不覆盖别人的关卡)。成功时 out 填关卡目录。
		static bool CreateEmpty(WorldContext& context, const std::filesystem::path& packageDir,
			const std::string& levelName, std::string* error = nullptr);

		// 由场景重建整包:逐实体走 SceneSerializer 的**实体级**序列化 —— 全部组件随文件走,
		// 数据层归属来自实体自身的 LevelEntityComponent。数据层定义(strings/masks)由调用方
		// 事先设好(不随场景走)。已有实体集合会被清空重建。
		void CaptureFromScene(Scene& scene, const std::string& levelName, uint32_t gridCellSize);

		// 把整包实体并入一个新场景。逐实体走 SceneSerializer 的实体级路径(全部组件),
		// 末尾 FinalizeEntityImport 统一重算 Transform 缓存矩阵与层级反向索引。
		//
		// 这是关卡包 → 场景的**唯一**实现:编辑器打开关卡、Runtime 加载关卡、测试夹具
		// 都走它,避免"编辑器能读、运行期丢组件"这类两条实现带来的偏差。
		Ref<Scene> Instantiate(WorldContext& context) const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
