#include "Document/EditorDocument.h"

#include "World/Core/Application.h"
#include "World/Core/Log.h"
#include "World/Level/Authoring/LevelCatalog.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Scene/SceneSerializer.h"

#include <unordered_map>

namespace World
{
	EditorDocument::EditorDocument(WorldContext& context)
		: m_Context(&context), m_Scene(CreateRef<Scene>(context))
	{
	}

	bool EditorDocument::LoadFromFile(const std::filesystem::path& path)
	{
		if (path.empty())
		{
			m_LastError = "Cannot load: empty path.";
			return false;
		}

		// 关卡入口 = `.wlevel` 文件(唯一形态;目录/清单嗅探已退役)。
		if (path.extension() == Level::LevelPackage::EntryExtension)
		{
			Level::LevelPackage package;
			std::string err;
			if (package.Load(path, &err))
			{
				Ref<Scene> newScene = package.Instantiate(*m_Context);

				m_Scene = newScene;
				m_Path = path;
				m_LevelPackagePath = path;
				m_DataLayers = package.GetDataLayers();
				m_Dirty = false;
				m_LastError.clear();
				WLD_CORE_INFO("[level] 关卡已载入: {0} ({1} 个实体, {2} 个数据层)",
					path.u8string(), package.GetEntityCount(), m_DataLayers.size());
				return true;
			}
			m_LastError = err;
			WLD_CORE_WARN("Level entry '{0}' rejected: {1}", path.u8string(), err);
			return false;
		}

		// 非关卡文件:仍走整场景序列化(预制体表单、以及历史遗留的单文件场景)。
		Ref<Scene> newScene = CreateRef<Scene>(*m_Context);
		SceneSerializer serializer(newScene);
		if (!serializer.Deserialize(path.string()))
		{
			m_LastError = serializer.GetLastError().empty()
				? "Failed to load: " + path.string()
				: serializer.GetLastError();
			return false;
		}

		m_Scene = newScene;
		m_Path = path;
		m_LevelPackagePath.clear();
		m_DataLayers.clear();
		m_Dirty = false;
		m_LastError.clear();
		return true;
	}

	bool EditorDocument::SaveTo(const std::filesystem::path& path)
	{
		if (path.empty())
		{
			m_LastError = "Cannot save: empty path.";
			return false;
		}

		// 关卡目标:显式 `.wlevel`,或当前文档本来就是关卡。
		const bool wantsLevel = (path.extension() == Level::LevelPackage::EntryExtension)
			|| IsLevelPackage();

		if (wantsLevel)
		{
			const std::filesystem::path entryPath = path;

			// 从**当前场景**重建实体集合:按 UUID 对齐保留既有 ChunkID;丢掉的实体自然被
			// LevelPackage::Save 的剪枝删除。数据层定义沿用原 manifest(不硬编码 Base_World)。
			std::unordered_map<uint64_t, uint64_t> chunkIds;
			Level::LevelPackage package;
			if (std::filesystem::exists(entryPath))
			{
				Level::LevelPackage existing;
				std::string existingError;
				if (existing.Load(entryPath, &existingError))
					for (size_t i = 0; i < existing.GetEntityCount(); ++i)
						chunkIds[existing.GetEntity(i).EntityUUID] = existing.GetEntity(i).ChunkID;
			}

			package.SetId(entryPath.stem().string());
			package.SetName(m_DataLayers.empty() && !package.GetName().empty()
				? package.GetName() : entryPath.stem().string());
			// 入口路径必须先设:CameraFromScene 的稀疏放置判定要用内容根解析实例引用的
			// `.wprefab`(见 LevelPackage::SetEntryPath)。
			package.SetEntryPath(entryPath);
			package.CaptureFromScene(*m_Scene, entryPath.stem().string(), 64);
			if (!m_DataLayers.empty())
				package.GetDataLayers() = m_DataLayers;
			else
				package.GetDataLayers().push_back({ 1u, "Base_World", true });

			for (size_t i = 0; i < package.GetEntityCount(); ++i)
			{
				Level::EntityDescriptor& descriptor = package.GetMutableEntity(i);
				if (const auto it = chunkIds.find(descriptor.EntityUUID); it != chunkIds.end())
					descriptor.ChunkID = it->second;
			}

			std::string err;
			if (!package.Save(entryPath, &err))
			{
				m_LastError = "Failed to save level: " + err;
				return false;
			}

			m_Path = entryPath;
			m_LevelPackagePath = entryPath;
			m_DataLayers = package.GetDataLayers();
			m_Dirty = false;
			m_LastError.clear();
			WLD_CORE_INFO("[level] 关卡已保存: {0} ({1} 个实体)", entryPath.u8string(), package.GetEntityCount());
			return true;
		}

		SceneSerializer serializer(m_Scene);
		if (!serializer.Serialize(path.string()))
		{
			m_LastError = serializer.GetLastError().empty()
				? "Failed to save: " + path.string()
				: serializer.GetLastError();
			return false;
		}

		m_Path = path;
		m_Dirty = false;
		m_LastError.clear();
		return true;
	}

	bool EditorDocument::SaveDataLayers(std::string* error)
	{
		if (!IsLevelPackage())
		{
			if (error) *error = "the open document is not a level (no data layers to save)";
			return false;
		}

		// 走 LevelPackage 的读写器:Load 拿回同一份 manifest(Id/Name/GridCellSize/World/实体引用
		// 都不动)+ 归一化层定义,然后**只**写入口文件(SaveEntry)—— 实体文件一个字节不碰。
		Level::LevelPackage package;
		std::string packageError;
		if (!package.Load(m_LevelPackagePath, &packageError))
		{
			if (error) *error = "level manifest could not be reopened: " + packageError;
			return false;
		}
		package.GetDataLayers() = m_DataLayers;
		if (!package.SaveEntry(&packageError))
		{
			if (error) *error = "level manifest could not be written: " + packageError;
			return false;
		}

		// 写回的是**归一化后**的定义(引擎侧规则),文档缓存与磁盘保持一致。
		m_DataLayers = package.GetDataLayers();
		return true;
	}

	void EditorDocument::New()
	{
		m_Scene = CreateRef<Scene>(*m_Context);
		m_Path.clear();
		m_LevelPackagePath.clear();
		m_DataLayers.clear();
		m_Dirty = false;
		m_LastError.clear();
	}
}
