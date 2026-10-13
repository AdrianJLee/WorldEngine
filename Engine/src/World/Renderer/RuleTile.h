#pragma once

#include "World/Core/Export.h"
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace World
{
	class Tilemap;

	// 8 邻域规则判定状态
	enum class RuleNeighbor : uint8_t
	{
		Ignore = 0,   // 无关 / 通配 (匹配任意状态)
		This = 1,     // 必须是本类瓦片 / 实体
		NotThis = 2   // 必须不是本类瓦片 / 空
	};

	// 单条规则项: 8 个方向的邻域约束 + 匹配成功时的目标瓦片 ID
	// 邻域索引顺序: 0:NW, 1:N, 2:NE, 3:W, 4:E, 5:SW, 6:S, 7:SE
	struct WLD_API RuleTileRule
	{
		uint8_t Neighbors[8] { 0 };
		uint16_t OutputTileId = 1;
	};

	class WLD_API RuleTile
	{
	public:
		RuleTile() = default;
		RuleTile(std::string name, std::string atlasPath, uint16_t defaultTileId = 1);

		const std::string& GetName() const noexcept { return m_Name; }
		void SetName(std::string name) { m_Name = std::move(name); }

		const std::string& GetAtlasPath() const noexcept { return m_AtlasPath; }
		void SetAtlasPath(std::string path) { m_AtlasPath = std::move(path); }

		uint16_t GetDefaultTileId() const noexcept { return m_DefaultTileId; }
		void SetDefaultTileId(uint16_t id) noexcept { m_DefaultTileId = id; }

		const std::vector<RuleTileRule>& GetRules() const noexcept { return m_Rules; }
		std::vector<RuleTileRule>& GetRules() noexcept { return m_Rules; }
		void AddRule(const RuleTileRule& rule) { m_Rules.push_back(rule); }
		void ClearRules() { m_Rules.clear(); }

		// 根据周围 8 格连通状态解析出此单元格应呈现的瓦片 ID
		uint16_t ResolveTile(const Tilemap& tilemap, int x, int y) const;

		// 生成经典的 47-Tile 行业标准全套自动拼接规则模板
		// baseTileId: 47 张瓦片在图集中的起始切片 ID (1..N)
		static RuleTile CreateStandard47Template(std::string name, std::string atlasPath, uint16_t baseTileId);

		std::string SerializeYaml() const;
		bool DeserializeYaml(const std::string& yaml);

		static std::shared_ptr<RuleTile> LoadFromFile(const std::string& filepath);
		static std::shared_ptr<RuleTile> GetOrLoad(const std::string& filepath);
		bool SaveToFile(const std::string& filepath) const;

	private:
		std::string m_Name { "NewRuleTile" };
		std::string m_AtlasPath;
		uint16_t m_DefaultTileId = 1;
		std::vector<RuleTileRule> m_Rules;
	};
}
