#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace World
{
	// P1b D5b:编辑器侧的资产类型元数据(纯展示;引擎/打包不依赖它)。
	// 内容浏览器与模型预览共用同一张表,避免两处各自硬编码扩展名分支。
	enum class EditorAssetKind
	{
		Unknown,
		Scene,
		Material,
		Model,          // .wmodel(引擎原生模型资产)
		ModelSource,    // .gltf/.glb(源资产;双击 = 导入 + 打开预览)
		Prefab,         // .wprefab(可复用实体子树;双击 = 打开编辑)
		Texture,
		Script,
		Folder,
	};

	struct EditorAssetType
	{
		EditorAssetKind Kind = EditorAssetKind::Unknown;
		const char* Name = "File";
	};

	inline std::string LowerExtension(const std::filesystem::path& path)
	{
		std::string extension = path.extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return extension;
	}

	inline EditorAssetType DescribeAssetType(const std::filesystem::path& path, bool isDirectory)
	{
		if (isDirectory)
			return { EditorAssetKind::Folder, "Folder" };
		const std::string extension = LowerExtension(path);
		if (extension == ".wd")
			return { EditorAssetKind::Scene, "Scene" };
		if (extension == ".wmat")
			return { EditorAssetKind::Material, "Material" };
		if (extension == ".wmodel")
			return { EditorAssetKind::Model, "Model" };
		if (extension == ".gltf" || extension == ".glb")
			return { EditorAssetKind::ModelSource, "glTF Source" };
		if (extension == ".wprefab")
			return { EditorAssetKind::Prefab, "Prefab" };
		if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga")
			return { EditorAssetKind::Texture, "Texture" };
		if (extension == ".lua" || extension == ".luau")
			return { EditorAssetKind::Script, "Script" };
		return { EditorAssetKind::Unknown, "File" };
	}
}
