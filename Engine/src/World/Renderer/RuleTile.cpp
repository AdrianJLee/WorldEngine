#include "wldpch.h"
#include "World/Renderer/RuleTile.h"
#include "World/Renderer/Tilemap.h"
#include "World/Core/Log.h"

#include <yaml-cpp/yaml.h>
#include <fstream>
#include <unordered_map>
#include <mutex>

namespace World
{
	namespace
	{
		static std::unordered_map<std::string, std::shared_ptr<RuleTile>> s_RuleTileCache;
		static std::mutex s_RuleTileCacheMutex;
	}

	RuleTile::RuleTile(std::string name, std::string atlasPath, uint16_t defaultTileId)
		: m_Name(std::move(name)), m_AtlasPath(std::move(atlasPath)), m_DefaultTileId(defaultTileId)
	{
	}

	uint16_t RuleTile::ResolveTile(const Tilemap& tilemap, int x, int y) const
	{
		// 8 邻域相对偏移: [0:NW, 1:N, 2:NE, 3:W, 4:E, 5:SW, 6:S, 7:SE]
		static const int dx[8] = { -1,  0,  1, -1,  1, -1,  0,  1 };
		static const int dy[8] = { -1, -1, -1,  0,  0,  1,  1,  1 };

		for (const auto& rule : m_Rules)
		{
			bool matched = true;
			for (int i = 0; i < 8; ++i)
			{
				const uint8_t req = rule.Neighbors[i];
				if (req == static_cast<uint8_t>(RuleNeighbor::Ignore))
					continue;

				const int nx = x + dx[i];
				const int ny = y + dy[i];
				bool isNeighborSolid = false;
				if (nx >= 0 && nx < static_cast<int>(tilemap.GetWidth()) &&
				    ny >= 0 && ny < static_cast<int>(tilemap.GetHeight()))
				{
					isNeighborSolid = tilemap.IsSolid(nx, ny) || (tilemap.GetTile(nx, ny) != 0);
				}

				if (req == static_cast<uint8_t>(RuleNeighbor::This) && !isNeighborSolid)
				{
					matched = false;
					break;
				}
				if (req == static_cast<uint8_t>(RuleNeighbor::NotThis) && isNeighborSolid)
				{
					matched = false;
					break;
				}
			}

			if (matched)
				return rule.OutputTileId;
		}

		return m_DefaultTileId;
	}

	RuleTile RuleTile::CreateStandard47Template(std::string name, std::string atlasPath, uint16_t baseTileId)
	{
		RuleTile rt(std::move(name), std::move(atlasPath), baseTileId);

		const uint8_t T = static_cast<uint8_t>(RuleNeighbor::This);
		const uint8_t N = static_cast<uint8_t>(RuleNeighbor::NotThis);
		const uint8_t X = static_cast<uint8_t>(RuleNeighbor::Ignore);

		// 1. 孤立地块 (上下左右皆空)
		rt.AddRule({ { X, N, X,  N, N,  X, N, X }, static_cast<uint16_t>(baseTileId + 15) });

		// 2. 外部四角 (Outer Corners: 忽略无关对角线，使 2x2、L形及任意矩形均能精准识别转角)
		rt.AddRule({ { X, N, X,  N, T,  X, T, X }, static_cast<uint16_t>(baseTileId + 1) }); // 左上外角
		rt.AddRule({ { X, N, X,  T, N,  X, T, X }, static_cast<uint16_t>(baseTileId + 2) }); // 右上外角
		rt.AddRule({ { X, T, X,  N, T,  X, N, X }, static_cast<uint16_t>(baseTileId + 3) }); // 左下外角
		rt.AddRule({ { X, T, X,  T, N,  X, N, X }, static_cast<uint16_t>(baseTileId + 4) }); // 右下外角

		// 单行/单列末端外角兜底
		rt.AddRule({ { X, N, X,  N, T,  X, N, X }, static_cast<uint16_t>(baseTileId + 1) }); // 水平左端
		rt.AddRule({ { X, N, X,  T, N,  X, N, X }, static_cast<uint16_t>(baseTileId + 2) }); // 水平右端
		rt.AddRule({ { X, N, X,  N, N,  X, T, X }, static_cast<uint16_t>(baseTileId + 1) }); // 垂直上端
		rt.AddRule({ { X, T, X,  N, N,  X, N, X }, static_cast<uint16_t>(baseTileId + 3) }); // 垂直下端

		// 3. 四个边缘 (Edges)
		rt.AddRule({ { X, N, X,  T, T,  X, T, X }, static_cast<uint16_t>(baseTileId + 5) }); // 上边缘
		rt.AddRule({ { X, T, X,  T, T,  X, N, X }, static_cast<uint16_t>(baseTileId + 6) }); // 下边缘
		rt.AddRule({ { X, T, X,  N, T,  X, T, X }, static_cast<uint16_t>(baseTileId + 7) }); // 左边缘
		rt.AddRule({ { X, T, X,  T, N,  X, T, X }, static_cast<uint16_t>(baseTileId + 8) }); // 右边缘

		// 4. 内部四角 (Inner Corners)
		rt.AddRule({ { N, T, T,  T, T,  T, T, T }, static_cast<uint16_t>(baseTileId + 9) });  // 左上内角
		rt.AddRule({ { T, T, N,  T, T,  T, T, T }, static_cast<uint16_t>(baseTileId + 10) }); // 右上内角
		rt.AddRule({ { T, T, T,  T, T,  N, T, T }, static_cast<uint16_t>(baseTileId + 11) }); // 左下内角
		rt.AddRule({ { T, T, T,  T, T,  T, T, N }, static_cast<uint16_t>(baseTileId + 12) }); // 右下内角

		// 5. 垂直管道 / 水平管道
		rt.AddRule({ { X, T, X,  N, N,  X, T, X }, static_cast<uint16_t>(baseTileId + 13) }); // 垂直柱
		rt.AddRule({ { X, N, X,  T, T,  X, N, X }, static_cast<uint16_t>(baseTileId + 14) }); // 水平道

		// 6. 默认中心全实心填充
		rt.AddRule({ { X, T, X,  T, T,  X, T, X }, baseTileId });

		return rt;
	}

	std::string RuleTile::SerializeYaml() const
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "Name" << YAML::Value << m_Name;
		out << YAML::Key << "AtlasPath" << YAML::Value << m_AtlasPath;
		out << YAML::Key << "DefaultTileId" << YAML::Value << m_DefaultTileId;

		out << YAML::Key << "Rules" << YAML::Value << YAML::BeginSeq;
		for (const auto& r : m_Rules)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "OutputTileId" << YAML::Value << r.OutputTileId;
			out << YAML::Key << "Neighbors" << YAML::Value << YAML::Flow << YAML::BeginSeq;
			for (int i = 0; i < 8; ++i)
				out << static_cast<int>(r.Neighbors[i]);
			out << YAML::EndSeq;
			out << YAML::EndMap;
		}
		out << YAML::EndSeq;
		out << YAML::EndMap;

		return out.c_str();
	}

	bool RuleTile::DeserializeYaml(const std::string& yaml)
	{
		try
		{
			YAML::Node root = YAML::Load(yaml);
			if (!root || !root.IsMap()) return false;

			if (root["Name"]) m_Name = root["Name"].as<std::string>();
			if (root["AtlasPath"]) m_AtlasPath = root["AtlasPath"].as<std::string>();
			if (root["DefaultTileId"]) m_DefaultTileId = root["DefaultTileId"].as<uint16_t>();

			m_Rules.clear();
			if (root["Rules"] && root["Rules"].IsSequence())
			{
				for (const auto& node : root["Rules"])
				{
					RuleTileRule rule;
					if (node["OutputTileId"])
						rule.OutputTileId = node["OutputTileId"].as<uint16_t>();
					if (node["Neighbors"] && node["Neighbors"].IsSequence())
					{
						int idx = 0;
						for (const auto& nNode : node["Neighbors"])
						{
							if (idx < 8)
								rule.Neighbors[idx++] = static_cast<uint8_t>(nNode.as<int>());
						}
					}
					m_Rules.push_back(rule);
				}
			}
			return true;
		}
		catch (const std::exception& e)
		{
			WLD_WARN("RuleTile::DeserializeYaml failed: {0}", e.what());
			return false;
		}
	}

	std::shared_ptr<RuleTile> RuleTile::LoadFromFile(const std::string& filepath)
	{
		std::ifstream file(filepath);
		if (!file.is_open()) return nullptr;

		std::string str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		auto rt = std::make_shared<RuleTile>();
		if (!rt->DeserializeYaml(str))
			return nullptr;
		return rt;
	}

	std::shared_ptr<RuleTile> RuleTile::GetOrLoad(const std::string& filepath)
	{
		std::lock_guard<std::mutex> lock(s_RuleTileCacheMutex);
		auto it = s_RuleTileCache.find(filepath);
		if (it != s_RuleTileCache.end() && it->second)
			return it->second;

		auto rt = LoadFromFile(filepath);
		if (rt)
			s_RuleTileCache[filepath] = rt;
		return rt;
	}

	bool RuleTile::SaveToFile(const std::string& filepath) const
	{
		std::ofstream file(filepath, std::ios::trunc);
		if (!file.is_open()) return false;
		file << SerializeYaml();
		return true;
	}
}
