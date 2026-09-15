#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Scene.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace World::Gameplay
{
	// P2a W8:存档服务(槽位 + 版本迁移 + SaveTrait 白名单)。
	// 设计要点:
	//  - 存档 = SaveHeader + Globals(全局块) + Entities(场景块,UUID 键 + field id 字段);
	//    场景块与 .wd 共用 schema 序列化器,因此**同一套 field id**将来直接供 P2b 热重载迁移使用;
	//  - 落盘在用户可写目录(%LOCALAPPDATA%/<ProjectId>/saves),打包后不依赖只读内容根;
	//  - 坏档保护:解析/迁移失败只报错返回,不覆盖原档、不改场景。
	struct SaveHeader
	{
		std::string ProjectId;
		uint32_t Version = 1;
		std::string LevelId;
		uint64_t Timestamp = 0;
	};

	struct SaveSlotInfo
	{
		uint32_t Slot = 0;
		SaveHeader Header;
		bool Valid = false;
		std::string Error;   // Valid=false 时的原因(坏档/缺失字段)
	};

	// 读档结果报告:面板/"失效引用可检出"的最小落点。
	struct SaveLoadReport
	{
		uint32_t EntitiesUpdated = 0;   // 按 UUID 命中并覆盖
		uint32_t EntitiesCreated = 0;   // 存档里有、当前场景没有 → 新建
		uint32_t ComponentsApplied = 0;
		uint32_t ComponentsFailed = 0;  // 字段读取失败(保留默认值并告警)
	};

	class WLD_API SaveService
	{
	public:
		static constexpr uint32_t CurrentVersion = 1;

		// 场景来源:宿主(编辑器/Runtime)提供"当前活动场景"。
		using SceneProvider = std::function<Scene*()>;
		// 迁移钩子:root 是已解析的存档文档(实现自解释;YAML 实现期望 YAML::Node*)。
		// 返回 false 视为迁移失败 → 整个 Load 失败并保留原档。
		using MigrationFn = std::function<bool(uint32_t fromVersion, void* root)>;

		SaveService(std::string projectId, SceneProvider scene);
		// 显式指定用户根(测试/工具用);不指定时用 %LOCALAPPDATA%/<ProjectId>。
		SaveService(std::string projectId, SceneProvider scene, std::filesystem::path userRoot);

		bool Save(uint32_t slot, const std::string& levelId = {});
		bool Load(uint32_t slot);
		bool Delete(uint32_t slot);
		std::vector<SaveSlotInfo> ListSaves() const;

		void RegisterMigration(uint32_t fromVersion, MigrationFn fn);
		// SaveTrait 白名单:默认已含 Transform;Game/模块可把自定义 gameplay 组件加进来。
		void RegisterSaveTrait(const std::string& schemaTypeName);
		bool IsSaveTrait(const std::string& schemaTypeName) const;

		// 全局块:类型化键值(进度/解锁/玩家数据)。
		struct GlobalValue
		{
			enum class Kind : uint8_t { Int, Float, Bool, String };
			Kind Type = Kind::Int;
			int64_t Int = 0;
			double Float = 0.0;
			bool Bool = false;
			std::string String;
		};
		void SetGlobal(const std::string& key, GlobalValue value);
		bool TryGetGlobal(const std::string& key, GlobalValue* out) const;
		void ClearGlobals() { m_Globals.clear(); }

		const std::filesystem::path& GetSaveRoot() const { return m_SaveRoot; }
		std::filesystem::path GetSlotPath(uint32_t slot) const;
		const std::string& GetLastError() const { return m_LastError; }
		const SaveLoadReport& GetLastLoadReport() const { return m_LastLoadReport; }

	private:
		bool WriteDocument(uint32_t slot, const std::string& levelId);
		bool ApplyDocument(uint32_t slot);

		std::string m_ProjectId;
		SceneProvider m_Scene;
		std::filesystem::path m_SaveRoot;
		std::map<std::string, GlobalValue> m_Globals;
		std::unordered_map<uint32_t, MigrationFn> m_Migrations;
		std::unordered_set<std::string> m_SaveTraits;
		SaveLoadReport m_LastLoadReport;
		mutable std::string m_LastError;
	};
}
