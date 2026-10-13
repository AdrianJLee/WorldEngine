#include "World/Renderer/RuleTile.h"
#include "World/Utils/Paths.h"
#include "Tilemap.h"
#include "World/Core/Log.h"

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <queue>
#include <sstream>
#include <unordered_map>

namespace World
{
	static std::unordered_map<std::string, std::shared_ptr<Tilemap>> s_TilemapCache;
	static std::mutex s_TilemapCacheMutex;



	namespace
	{
		std::filesystem::path ResolvePhysicalPath(const std::string& filepath)
		{
			if (filepath.empty())
				return {};
			std::error_code ec;
			std::filesystem::path p(filepath);
			if (std::filesystem::exists(p, ec))
				return p;
			std::filesystem::path fromProj = Paths::ProjectDir() / p;
			if (std::filesystem::exists(fromProj, ec))
				return fromProj;
			std::filesystem::path fromAsset = Paths::AssetRoot() / p;
			if (std::filesystem::exists(fromAsset, ec))
				return fromAsset;
			if (filepath.rfind("assets/", 0) == 0 || filepath.rfind("assets\\", 0) == 0)
			{
				std::filesystem::path stripped = Paths::AssetRoot() / filepath.substr(7);
				if (std::filesystem::exists(stripped, ec))
					return stripped;
			}
			return p;
		}
	}

	Tilemap::Tilemap(std::string name, uint32_t width, uint32_t height, std::string atlasPath)
		: m_Name(std::move(name))
		, m_AtlasPath(std::move(atlasPath))
		, m_Width(std::max(1u, width))
		, m_Height(std::max(1u, height))
	{
		const size_t total = static_cast<size_t>(m_Width) * m_Height;
		m_Tiles.assign(total, 0);
		m_CollisionMask.assign(total, 0);
	}

	void Tilemap::Resize(uint32_t width, uint32_t height)
	{
		const uint32_t newW = std::max(1u, width);
		const uint32_t newH = std::max(1u, height);
		const size_t newTotal = static_cast<size_t>(newW) * newH;

		std::vector<uint16_t> newTiles(newTotal, 0);
		std::vector<uint8_t> newMask(newTotal, 0);

		const uint32_t copyW = std::min(m_Width, newW);
		const uint32_t copyH = std::min(m_Height, newH);
		for (uint32_t y = 0; y < copyH; ++y)
		{
			for (uint32_t x = 0; x < copyW; ++x)
			{
				const size_t dstIdx = static_cast<size_t>(y) * newW + x;
				newTiles[dstIdx] = GetTile(x, y);
				newMask[dstIdx] = IsSolid(x, y) ? 1u : 0u;
			}
		}

		m_Width = newW;
		m_Height = newH;
		m_Tiles = std::move(newTiles);
		m_CollisionMask = std::move(newMask);
	}

	void Tilemap::SetTile(uint32_t x, uint32_t y, uint16_t tileId)
	{
		if (x >= m_Width || y >= m_Height)
			return;
		const size_t total = static_cast<size_t>(m_Width) * m_Height;
		if (m_Tiles.size() < total)
			m_Tiles.resize(total, 0);
		const size_t idx = static_cast<size_t>(y) * m_Width + x;
		m_Tiles[idx] = tileId;
	}

	uint16_t Tilemap::GetTile(uint32_t x, uint32_t y) const noexcept
	{
		if (x >= m_Width || y >= m_Height)
			return 0;
		const size_t idx = static_cast<size_t>(y) * m_Width + x;
		return idx < m_Tiles.size() ? m_Tiles[idx] : 0;
	}

	void Tilemap::SetSolid(uint32_t x, uint32_t y, bool solid)
	{
		if (x >= m_Width || y >= m_Height)
			return;
		const size_t total = static_cast<size_t>(m_Width) * m_Height;
		if (m_CollisionMask.size() < total)
			m_CollisionMask.resize(total, 0);
		const size_t idx = static_cast<size_t>(y) * m_Width + x;
		m_CollisionMask[idx] = solid ? 1u : 0u;
	}

	bool Tilemap::IsSolid(uint32_t x, uint32_t y) const noexcept
	{
		if (x >= m_Width || y >= m_Height)
			return false;
		const size_t idx = static_cast<size_t>(y) * m_Width + x;
		return idx < m_CollisionMask.size() && m_CollisionMask[idx] != 0;
	}

	void Tilemap::Clear()
	{
		const size_t total = static_cast<size_t>(m_Width) * m_Height;
		m_Tiles.assign(total, 0);
		m_CollisionMask.assign(total, 0);
	}

	void Tilemap::FillRect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint16_t tileId, bool setSolid)
	{
		if (m_Width == 0 || m_Height == 0)
			return;

		uint32_t minX = std::min(x0, x1);
		uint32_t maxX = std::max(x0, x1);
		uint32_t minY = std::min(y0, y1);
		uint32_t maxY = std::max(y0, y1);
		if (minX >= m_Width || minY >= m_Height)
			return;

		maxX = std::min(maxX, m_Width - 1);
		maxY = std::min(maxY, m_Height - 1);

		for (uint32_t y = minY; y <= maxY; ++y)
		{
			for (uint32_t x = minX; x <= maxX; ++x)
			{
				SetTile(x, y, tileId);
				SetSolid(x, y, setSolid);
			}
		}
	}

	void Tilemap::FloodFill(uint32_t startX, uint32_t startY, uint16_t newTileId, bool setSolid)
	{
		if (startX >= m_Width || startY >= m_Height)
			return;

		const uint16_t targetTile = GetTile(startX, startY);
		const bool targetSolid = IsSolid(startX, startY);
		if (targetTile == newTileId && targetSolid == setSolid)
			return;

		const size_t total = static_cast<size_t>(m_Width) * m_Height;
		std::vector<uint8_t> visited(total, 0);
		std::queue<std::pair<uint32_t, uint32_t>> q;

		visited[static_cast<size_t>(startY) * m_Width + startX] = 1;
		q.push({ startX, startY });

		while (!q.empty())
		{
			const auto [cx, cy] = q.front();
			q.pop();

			SetTile(cx, cy, newTileId);
			SetSolid(cx, cy, setSolid);

			const int32_t dx[4] = { -1, 1, 0, 0 };
			const int32_t dy[4] = { 0, 0, -1, 1 };
			for (int i = 0; i < 4; ++i)
			{
				const int32_t nx = static_cast<int32_t>(cx) + dx[i];
				const int32_t ny = static_cast<int32_t>(cy) + dy[i];
				if (nx < 0 || ny < 0 || nx >= static_cast<int32_t>(m_Width) || ny >= static_cast<int32_t>(m_Height))
					continue;

				const uint32_t ux = static_cast<uint32_t>(nx);
				const uint32_t uy = static_cast<uint32_t>(ny);
				const size_t nIdx = static_cast<size_t>(uy) * m_Width + ux;
				if (!visited[nIdx] && GetTile(ux, uy) == targetTile)
				{
					visited[nIdx] = 1;
					q.push({ ux, uy });
				}
			}
		}
	}

	uint32_t Tilemap::CountPaintedTiles() const noexcept
	{
		uint32_t count = 0;
		const size_t limit = std::min(m_Tiles.size(), static_cast<size_t>(m_Width) * m_Height);
		for (size_t i = 0; i < limit; ++i)
		{
			if (m_Tiles[i] != 0)
				++count;
		}
		return count;
	}

	uint32_t Tilemap::CountSolidTiles() const noexcept
	{
		uint32_t count = 0;
		const size_t limit = std::min(m_CollisionMask.size(), static_cast<size_t>(m_Width) * m_Height);
		for (size_t i = 0; i < limit; ++i)
		{
			if (m_CollisionMask[i] != 0)
				++count;
		}
		return count;
	}

	std::vector<MergedColliderRect> Tilemap::ComputeMergedColliderRects() const
	{
		std::vector<MergedColliderRect> rects;
		if (m_Width == 0 || m_Height == 0)
			return rects;

		const size_t total = static_cast<size_t>(m_Width) * m_Height;
		std::vector<uint8_t> visited(total, 0);

		for (uint32_t y = 0; y < m_Height; ++y)
		{
			for (uint32_t x = 0; x < m_Width; ++x)
			{
				const size_t idx = static_cast<size_t>(y) * m_Width + x;
				if (!IsSolid(x, y) || visited[idx])
					continue;

				// 1. Expand maximum width to the right
				uint32_t w = 1;
				while (x + w < m_Width &&
					IsSolid(x + w, y) &&
					!visited[static_cast<size_t>(y) * m_Width + (x + w)])
				{
					++w;
				}

				// 2. Expand maximum height upwards (+Y)
				uint32_t h = 1;
				while (y + h < m_Height)
				{
					const uint32_t ny = y + h;
					bool rowAllSolid = true;
					for (uint32_t nx = x; nx < x + w; ++nx)
					{
						if (!IsSolid(nx, ny) || visited[static_cast<size_t>(ny) * m_Width + nx])
						{
							rowAllSolid = false;
							break;
						}
					}
					if (!rowAllSolid)
						break;
					++h;
				}

				// 3. Mark merged block [x..x+w-1, y..y+h-1] as visited
				for (uint32_t ny = y; ny < y + h; ++ny)
				{
					for (uint32_t nx = x; nx < x + w; ++nx)
					{
						visited[static_cast<size_t>(ny) * m_Width + nx] = 1;
					}
				}

				rects.push_back(MergedColliderRect{
					static_cast<float>(x),
					static_cast<float>(y),
					static_cast<float>(w),
					static_cast<float>(h)
				});
			}
		}

		return rects;
	}

	std::vector<std::vector<glm::vec2>> Tilemap::ComputeCompositeChainContours() const
	{
		std::vector<std::vector<glm::vec2>> contours;
		if (m_Width == 0 || m_Height == 0)
			return contours;

		struct DirectedEdge
		{
			glm::ivec2 A;
			glm::ivec2 B;
		};

		std::vector<DirectedEdge> edges;
		edges.reserve(static_cast<size_t>(m_Width) * m_Height);

		for (uint32_t y = 0; y < m_Height; ++y)
		{
			const int iy = static_cast<int>(y);
			for (uint32_t x = 0; x < m_Width; ++x)
			{
				if (!IsSolid(x, y))
					continue;

				const int ix = static_cast<int>(x);

				// Bottom edge: (x, y) -> (x+1, y) (when y == 0 || !IsSolid(x, y-1))
				if (y == 0 || !IsSolid(x, y - 1))
					edges.push_back({ { ix, iy }, { ix + 1, iy } });

				// Right edge: (x+1, y) -> (x+1, y+1) (when x == Width-1 || !IsSolid(x+1, y))
				if (x == m_Width - 1 || !IsSolid(x + 1, y))
					edges.push_back({ { ix + 1, iy }, { ix + 1, iy + 1 } });

				// Top edge: (x+1, y+1) -> (x, y+1) (when y == Height-1 || !IsSolid(x, y+1))
				if (y == m_Height - 1 || !IsSolid(x, y + 1))
					edges.push_back({ { ix + 1, iy + 1 }, { ix, iy + 1 } });

				// Left edge: (x, y+1) -> (x, y) (when x == 0 || !IsSolid(x-1, y))
				if (x == 0 || !IsSolid(x - 1, y))
					edges.push_back({ { ix, iy + 1 }, { ix, iy } });
			}
		}

		if (edges.empty())
			return contours;

		auto packVertex = [](const glm::ivec2& v) -> uint64_t
		{
			return (static_cast<uint64_t>(static_cast<uint32_t>(v.x)) << 32) | static_cast<uint64_t>(static_cast<uint32_t>(v.y));
		};

		std::unordered_map<uint64_t, std::vector<glm::ivec2>> outgoing;
		for (const auto& e : edges)
		{
			outgoing[packVertex(e.A)].push_back(e.B);
		}

		while (!outgoing.empty())
		{
			auto it = outgoing.begin();
			while (it != outgoing.end() && it->second.empty())
			{
				it = outgoing.erase(it);
			}
			if (it == outgoing.end())
				break;

			const glm::ivec2 startA = glm::ivec2(
				static_cast<int>(static_cast<int32_t>(it->first >> 32)),
				static_cast<int>(static_cast<int32_t>(it->first & 0xFFFFFFFFULL))
			);
			const glm::ivec2 startB = it->second.back();
			it->second.pop_back();
			if (it->second.empty())
				outgoing.erase(it);

			std::vector<glm::ivec2> rawLoop;
			rawLoop.push_back(startA);

			glm::ivec2 prev = startA;
			glm::ivec2 cur = startB;
			size_t stepCount = 0;
			const size_t maxSteps = edges.size() + 4;

			while (cur != startA && stepCount < maxSteps)
			{
				rawLoop.push_back(cur);
				++stepCount;

				const uint64_t curKey = packVertex(cur);
				auto nextIt = outgoing.find(curKey);
				if (nextIt == outgoing.end() || nextIt->second.empty())
					break;

				// If multiple outgoing edges (e.g. diagonal checkerboard corner), prefer leftmost CCW turn
				size_t bestIdx = 0;
				if (nextIt->second.size() > 1)
				{
					const glm::ivec2 inDir = cur - prev;
					int bestScore = -999;
					for (size_t k = 0; k < nextIt->second.size(); ++k)
					{
						const glm::ivec2 outDir = nextIt->second[k] - cur;
						const int cross = inDir.x * outDir.y - inDir.y * outDir.x;
						const int dot = inDir.x * outDir.x + inDir.y * outDir.y;
						int score = 0;
						if (cross > 0) score = 3;
						else if (cross == 0 && dot > 0) score = 2;
						else if (cross < 0) score = 1;
						if (score > bestScore)
						{
							bestScore = score;
							bestIdx = k;
						}
					}
				}

				const glm::ivec2 nextPt = nextIt->second[bestIdx];
				nextIt->second.erase(nextIt->second.begin() + bestIdx);
				if (nextIt->second.empty())
					outgoing.erase(nextIt);

				prev = cur;
				cur = nextPt;
			}

			// Simplify collinear points along closed polygon loop
			if (rawLoop.size() >= 3)
			{
				std::vector<glm::ivec2> simplified;
				const size_t N = rawLoop.size();
				for (size_t i = 0; i < N; ++i)
				{
					const glm::ivec2& p0 = rawLoop[(i + N - 1) % N];
					const glm::ivec2& p1 = rawLoop[i];
					const glm::ivec2& p2 = rawLoop[(i + 1) % N];

					const glm::ivec2 d1 = p1 - p0;
					const glm::ivec2 d2 = p2 - p1;
					const int cross = d1.x * d2.y - d1.y * d2.x;
					const int dot = d1.x * d2.x + d1.y * d2.y;

					if (cross == 0 && dot > 0)
						continue; // Collinear, remove intermediate vertex

					simplified.push_back(p1);
				}

				if (simplified.size() >= 3)
			{
					std::vector<glm::vec2> contour;
					contour.reserve(simplified.size());
					for (const auto& pt : simplified)
					{
						contour.emplace_back(
							static_cast<float>(pt.x) * m_CellSize.x,
							static_cast<float>(pt.y) * m_CellSize.y
						);
					}
					contours.push_back(std::move(contour));
				}
			}
		}

		return contours;
	}

	std::string Tilemap::SerializeYaml() const
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "Tilemap" << YAML::Value << m_Name;
		out << YAML::Key << "AtlasPath" << YAML::Value << m_AtlasPath;
	out << YAML::Key << "GridType" << YAML::Value << TilemapGridUtils::GetGridTypeName(m_GridType);
		out << YAML::Key << "Width" << YAML::Value << m_Width;
		out << YAML::Key << "Height" << YAML::Value << m_Height;
		out << YAML::Key << "CellSize" << YAML::Flow << YAML::BeginSeq
			<< m_CellSize.x << m_CellSize.y
			<< YAML::EndSeq;

		out << YAML::Key << "Tiles" << YAML::Flow << YAML::BeginSeq;
		for (uint32_t y = 0; y < m_Height; ++y)
		{
			for (uint32_t x = 0; x < m_Width; ++x)
			{
				out << static_cast<uint32_t>(GetTile(x, y));
			}
		}
		out << YAML::EndSeq;

		out << YAML::Key << "CollisionMask" << YAML::Flow << YAML::BeginSeq;
		for (uint32_t y = 0; y < m_Height; ++y)
		{
			for (uint32_t x = 0; x < m_Width; ++x)
			{
				out << static_cast<int>(IsSolid(x, y) ? 1 : 0);
			}
		}
		out << YAML::EndSeq;

		out << YAML::EndMap;
		return std::string(out.c_str());
	}

	bool Tilemap::DeserializeYaml(const std::string& yaml)
	{
		try
		{
			YAML::Node data = YAML::Load(yaml);
			if (!data || !data.IsMap())
				return false;

			if (data["Tilemap"])
				m_Name = data["Tilemap"].as<std::string>();
			else if (data["Name"])
				m_Name = data["Name"].as<std::string>();

			if (data["AtlasPath"])
				m_AtlasPath = data["AtlasPath"].as<std::string>();
			if (data["GridType"])
				m_GridType = TilemapGridUtils::ParseGridTypeName(data["GridType"].as<std::string>().c_str());
			if (data["Width"])
				m_Width = std::max(1u, data["Width"].as<uint32_t>());
			if (data["Height"])
				m_Height = std::max(1u, data["Height"].as<uint32_t>());

			if (data["CellSize"] && data["CellSize"].IsSequence() && data["CellSize"].size() == 2)
			{
				m_CellSize.x = data["CellSize"][0].as<float>();
				m_CellSize.y = data["CellSize"][1].as<float>();
			}

			const size_t total = static_cast<size_t>(m_Width) * m_Height;
			m_Tiles.assign(total, 0);
			m_CollisionMask.assign(total, 0);

			auto tilesNode = data["Tiles"];
			if (tilesNode && tilesNode.IsSequence())
			{
				const size_t count = std::min(tilesNode.size(), total);
				for (size_t i = 0; i < count; ++i)
				{
					m_Tiles[i] = tilesNode[i].as<uint16_t>();
				}
			}

			auto maskNode = data["CollisionMask"];
			if (maskNode && maskNode.IsSequence())
			{
				const size_t count = std::min(maskNode.size(), total);
				for (size_t i = 0; i < count; ++i)
				{
					m_CollisionMask[i] = static_cast<uint8_t>(maskNode[i].as<int>() != 0 ? 1u : 0u);
				}
			}

			return true;
		}
		catch (const std::exception& e)
		{
			WLD_CORE_WARN("[Tilemap] Failed to deserialize YAML: {}", e.what());
			return false;
		}
	}

	std::shared_ptr<Tilemap> Tilemap::LoadFromFile(const std::string& filepath)
	{
		const std::string physicalPath = ResolvePhysicalPath(filepath).string();
		std::ifstream file(physicalPath);
		if (!file.is_open())
		{
			WLD_CORE_WARN("[Tilemap] Failed to open file: {}", filepath);
			return nullptr;
		}
		std::stringstream ss;
		ss << file.rdbuf();
		auto tm = std::make_shared<Tilemap>();
		if (!tm->DeserializeYaml(ss.str()))
		{
			return nullptr;
		}
		return tm;
	}

	std::shared_ptr<Tilemap> Tilemap::GetOrLoad(const std::string& filepath)
	{
		std::lock_guard<std::mutex> lock(s_TilemapCacheMutex);
		auto it = s_TilemapCache.find(filepath);
		if (it != s_TilemapCache.end())
			return it->second;

		auto tm = LoadFromFile(filepath);
		if (tm)
		{
			s_TilemapCache[filepath] = tm;
		}
		return tm;
	}

	void Tilemap::ClearCache()
	{
		std::lock_guard<std::mutex> lock(s_TilemapCacheMutex);
		s_TilemapCache.clear();
	}

	
	void Tilemap::ApplyRuleTile(const RuleTile& ruleTile)
	{
		for (uint32_t y = 0; y < m_Height; ++y)
		{
			for (uint32_t x = 0; x < m_Width; ++x)
			{
				if (IsSolid(x, y) || GetTile(x, y) != 0)
				{
					const uint16_t resolvedId = ruleTile.ResolveTile(*this, static_cast<int>(x), static_cast<int>(y));
					SetTile(x, y, resolvedId);
				}
			}
		}
	}

	void Tilemap::ApplyRuleTileAt(int x, int y, const RuleTile& ruleTile)
	{
		if (x < 0 || x >= static_cast<int>(m_Width) || y < 0 || y >= static_cast<int>(m_Height))
			return;

		static const int dx[9] = { 0, -1,  0,  1, -1,  1, -1,  0,  1 };
		static const int dy[9] = { 0, -1, -1, -1,  0,  0,  1,  1,  1 };

		for (int i = 0; i < 9; ++i)
		{
			const int nx = x + dx[i];
			const int ny = y + dy[i];
			if (nx >= 0 && nx < static_cast<int>(m_Width) &&
			    ny >= 0 && ny < static_cast<int>(m_Height))
			{
				if (IsSolid(nx, ny) || GetTile(nx, ny) != 0)
				{
					const uint16_t resolvedId = ruleTile.ResolveTile(*this, nx, ny);
					SetTile(static_cast<uint32_t>(nx), static_cast<uint32_t>(ny), resolvedId);
				}
			}
		}
	}

	bool Tilemap::SaveToFile(const std::string& filepath) const
	{
		const std::string physicalPath = ResolvePhysicalPath(filepath).string();
		const std::filesystem::path fsPath(physicalPath);
		if (fsPath.has_parent_path())
		{
			std::error_code ec;
			std::filesystem::create_directories(fsPath.parent_path(), ec);
		}

		std::ofstream file(physicalPath);
		if (!file.is_open())
		{
			WLD_CORE_WARN("[Tilemap] Failed to write file: {}", filepath);
			return false;
		}
		file << SerializeYaml();
		return true;
	}
}
