#include "wldpch.h"
#include "World/Level/Cooking/LevelCooker.h"

#include "World/Level/Core/ChunkIndex.h"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace World::Level
{
	struct ChunkBucket
	{
		uint64_t ChunkID{ 0 };
		std::vector<size_t> EntityIndices;
	};

	CookResult LevelCooker::CookPackage(
		const LevelPackage& package,
		const std::filesystem::path& outputDir,
		const CookSettings& settings)
	{
		CookResult result;

		try
		{
			std::filesystem::create_directories(outputDir);

			// 流送索引:发行包里没有目录遍历,运行期靠它知道"有哪几块、在哪、包围盒多大"。
			ChunkIndex chunkIndex;

			const float defaultCellSize = static_cast<float>(settings.GridCellSize > 0 ? settings.GridCellSize : (package.GetGridCellSize() > 0 ? package.GetGridCellSize() : 64));
			const PartitionGridPlane gridPlane = package.GetPartitionSettings().GridPlane;

			// 收集场景中定义的局部特化特定区域 Volume (PartitionVolumeComponent)
			struct LocalVolume
			{
				glm::vec3 Center{ 0.0f };
				glm::vec3 HalfExtents{ 50.0f, 25.0f, 50.0f };
				uint32_t OverrideCellSize{ 16 };
			};
			std::vector<LocalVolume> localVolumes;
			for (size_t i = 0; i < package.GetEntityCount(); ++i)
			{
				const auto& ent = package.GetEntity(i);
				if (ent.DocumentYaml.find("PartitionVolumeComponent") != std::string::npos)
				{
					try
					{
						YAML::Node node = YAML::Load(ent.DocumentYaml);
						if (node["Entities"] && node["Entities"][0] && node["Entities"][0]["World::PartitionVolumeComponent"])
						{
							const auto& pv = node["Entities"][0]["World::PartitionVolumeComponent"];
							LocalVolume vol;
							vol.Center = ent.Position;
							if (pv["HalfExtents"] && pv["HalfExtents"].IsSequence())
								vol.HalfExtents = glm::vec3(pv["HalfExtents"][0].as<float>(50.0f), pv["HalfExtents"][1].as<float>(25.0f), pv["HalfExtents"][2].as<float>(50.0f));
							if (pv["OverrideCellSize"])
								vol.OverrideCellSize = pv["OverrideCellSize"].as<uint32_t>(16);
							localVolumes.push_back(vol);
						}
					}
					catch (...) {}
				}
			}

			std::vector<ChunkBucket> chunkBuckets;

			for (size_t i = 0; i < package.GetEntityCount(); ++i)
			{
				const auto& entity = package.GetEntity(i);
				uint64_t chunkId = entity.ChunkID;
				if (chunkId == 0)
				{
					// 判断实体是否落在某个特定区域 Volume 内
					float activeCellSize = defaultCellSize;
					for (const auto& vol : localVolumes)
					{
						const glm::vec3 delta = glm::abs(entity.Position - vol.Center);
						if (delta.x <= vol.HalfExtents.x && delta.y <= vol.HalfExtents.y && delta.z <= vol.HalfExtents.z)
						{
							activeCellSize = static_cast<float>(vol.OverrideCellSize > 0 ? vol.OverrideCellSize : 16);
							break;
						}
					}

					// 根据 5 大切面模式精确计算 ChunkID
					if (gridPlane == PartitionGridPlane::VerticalXY)
					{
						const int32_t cx = static_cast<int32_t>(std::floor(entity.Position.x / activeCellSize));
						const int32_t cy = static_cast<int32_t>(std::floor(entity.Position.y / activeCellSize));
						chunkId = (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint64_t>(static_cast<uint32_t>(cy));
					}
					else if (gridPlane == PartitionGridPlane::VerticalZY)
					{
						const int32_t cz = static_cast<int32_t>(std::floor(entity.Position.z / activeCellSize));
						const int32_t cy = static_cast<int32_t>(std::floor(entity.Position.y / activeCellSize));
						chunkId = (static_cast<uint64_t>(static_cast<uint32_t>(cz)) << 32) | static_cast<uint64_t>(static_cast<uint32_t>(cy));
					}
					else if (gridPlane == PartitionGridPlane::Isometric)
					{
						const float isoX = (entity.Position.x - entity.Position.z) * 0.70710678f;
						const float isoZ = (entity.Position.x + entity.Position.z) * 0.70710678f;
						const int32_t cx = static_cast<int32_t>(std::floor(isoX / activeCellSize));
						const int32_t cz = static_cast<int32_t>(std::floor(isoZ / activeCellSize));
						chunkId = (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint64_t>(static_cast<uint32_t>(cz));
					}
					else if (gridPlane == PartitionGridPlane::Full3D)
					{
						const int32_t cx = static_cast<int32_t>(std::floor(entity.Position.x / activeCellSize));
						const int32_t cy = static_cast<int32_t>(std::floor(entity.Position.y / activeCellSize));
						const int32_t cz = static_cast<int32_t>(std::floor(entity.Position.z / activeCellSize));
						chunkId = (static_cast<uint64_t>(static_cast<uint32_t>(cx) & 0x1FFFFFu) << 42)
						        | (static_cast<uint64_t>(static_cast<uint32_t>(cy) & 0x1FFFFFu) << 21)
						        | static_cast<uint64_t>(static_cast<uint32_t>(cz) & 0x1FFFFFu);
					}
					else // HorizontalXZ (3D大世界 [默认])
					{
						const int32_t cx = static_cast<int32_t>(std::floor(entity.Position.x / activeCellSize));
						const int32_t cz = static_cast<int32_t>(std::floor(entity.Position.z / activeCellSize));
						chunkId = (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint64_t>(static_cast<uint32_t>(cz));
					}
				}

				auto it = std::find_if(chunkBuckets.begin(), chunkBuckets.end(), [chunkId](const ChunkBucket& b) {
					return b.ChunkID == chunkId;
				});

				if (it == chunkBuckets.end())
				{
					chunkBuckets.push_back({ chunkId, { i } });
				}
				else
				{
					it->EntityIndices.push_back(i);
				}
			}

			for (const auto& bucket : chunkBuckets)
			{
				const uint64_t chunkId = bucket.ChunkID;
				const auto& indices = bucket.EntityIndices;

				ChunkHeader header;
				header.Magic = ChunkMagic;
				header.Version = ChunkVersion;
				header.ChunkID = chunkId;
				header.SubspaceID = settings.SubspaceID;
				header.StaticEntityCount = static_cast<uint32_t>(indices.size());
				header.BlobOffset = sizeof(ChunkHeader);
				header.BlobSize = indices.size() * sizeof(StaticEntityRecord);

				glm::vec3 minBound{ 1e9f };
				glm::vec3 maxBound{ -1e9f };

				std::unique_ptr<StaticEntityRecord[]> records = std::make_unique<StaticEntityRecord[]>(indices.size());

				for (size_t r = 0; r < indices.size(); ++r)
				{
					const auto& ent = package.GetEntity(indices[r]);
					StaticEntityRecord& rec = records[r];
					rec.EntityUUID = ent.EntityUUID;
					rec.DataLayerMask = ent.DataLayerMask;
					rec.Position = ent.Position;
					rec.RotationEuler = ent.RotationEuler;
					rec.ScaleX = ent.Scale.x;
					rec.ScaleY = ent.Scale.y;
					rec.ScaleZ = ent.Scale.z;

					std::strncpy(rec.Name, ent.Name.c_str(), sizeof(rec.Name) - 1);
					rec.Name[sizeof(rec.Name) - 1] = '\0';

					minBound = glm::min(minBound, ent.Position);
					maxBound = glm::max(maxBound, ent.Position);
				}

				if (indices.empty())
				{
					minBound = glm::vec3(0.0f);
					maxBound = glm::vec3(0.0f);
				}

				header.BoundsMin = minBound;
				header.BoundsMax = maxBound;

				// 实体文档段:每个静态实体一份**完整组件文档**(与 `.entity` 同源)。
				// 记录表给空间切分用,文档段才是"载入后场景真的长这样"的载荷 ——
				// 只有记录表的话,载入结果是一堆命名空壳。
				std::vector<ChunkDocumentEntry> documents(indices.size());
				std::string blob;
				{
					for (size_t r = 0; r < indices.size(); ++r)
					{
						const auto& ent = package.GetEntity(indices[r]);
						documents[r].EntityUUID = ent.EntityUUID;
						documents[r].Size = static_cast<uint32_t>(ent.DocumentYaml.size());
					}
					size_t cursor = 0;
					for (size_t r = 0; r < indices.size(); ++r)
						cursor += documents[r].Size;
					blob.resize(cursor);
					cursor = 0;
					for (size_t r = 0; r < indices.size(); ++r)
					{
						const auto& ent = package.GetEntity(indices[r]);
						if (!ent.DocumentYaml.empty())
							std::memcpy(blob.data() + cursor, ent.DocumentYaml.data(), ent.DocumentYaml.size());
						cursor += ent.DocumentYaml.size();
					}
				}

				header.DocumentOffset = sizeof(ChunkHeader)
					+ indices.size() * sizeof(StaticEntityRecord);
				header.DocumentSize = indices.size() * sizeof(ChunkDocumentEntry) + blob.size();

				// 各条目的 YAML 绝对偏移:文档表之后依次摆放。
				{
					size_t cursor = header.DocumentOffset + indices.size() * sizeof(ChunkDocumentEntry);
					for (size_t r = 0; r < indices.size(); ++r)
					{
						documents[r].Offset = cursor;
						cursor += documents[r].Size;
					}
				}

				const std::string chunkFileName = std::to_string(chunkId) + ".wchunk";
				const std::filesystem::path chunkFilePath = outputDir / chunkFileName;

				std::ofstream fout(chunkFilePath, std::ios::binary);
				if (!fout.is_open())
				{
					result.Success = false;
					result.ErrorMessage = "Failed to write chunk file: " + chunkFilePath.string();
					return result;
				}

				fout.write(reinterpret_cast<const char*>(&header), sizeof(ChunkHeader));
				if (!indices.empty())
				{
					fout.write(reinterpret_cast<const char*>(records.get()), indices.size() * sizeof(StaticEntityRecord));
					fout.write(reinterpret_cast<const char*>(documents.data()),
						documents.size() * sizeof(ChunkDocumentEntry));
				}
				if (!blob.empty())
					fout.write(blob.data(), static_cast<std::streamsize>(blob.size()));
				fout.close();

				ChunkIndexEntry indexEntry;
				indexEntry.ChunkID = chunkId;
				indexEntry.File = chunkFileName;
				indexEntry.Bounds = ChunkBounds{ minBound, maxBound };
				indexEntry.EntityCount = static_cast<uint32_t>(indices.size());
				for (size_t r = 0; r < indices.size(); ++r)
					indexEntry.DataLayerMask |= package.GetEntity(indices[r]).DataLayerMask;
				chunkIndex.Entries().push_back(std::move(indexEntry));

				result.OutputChunkFiles.push_back(chunkFilePath);
				result.ChunksCooked++;
				result.TotalEntitiesCooked += static_cast<uint32_t>(indices.size());
			}

			// 索引最后写:块全写成功才留下索引,避免"索引说有三块、盘上只有两块"。
			std::string indexError;
			if (!ChunkIndex::Save(outputDir / ChunkIndex::FileName, chunkIndex, &indexError))
			{
				result.Success = false;
				result.ErrorMessage = "Failed to write chunk index: " + indexError;
				return result;
			}

			result.Success = true;
			return result;
		}
		catch (const std::exception& e)
		{
			result.Success = false;
			result.ErrorMessage = e.what();
			return result;
		}
	}
}
