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
		// Slang-B1:表面函数材质(Material Shader)—— 唯一扩展名 `.slang`。
		// 参数与默认值写在文件注解里。
		Shader,
		Model,          // .wmodel(引擎原生模型资产)
		ModelSource,    // .gltf/.glb(源资产;双击 = 导入 + 打开预览)
		Prefab,         // .wprefab(可复用实体子树;双击 = 打开编辑)
		// M4-TEX P4:纹理是"源 + 资产"两件 —— 与 `.gltf`/`.wmodel` 同款关系:
		//   TextureSource = png/jpg/jpeg/tga/bmp(解码输入;**材质引用的就是它**的路径)
		//   TextureAsset  = .wtex(导入设置 + `source:` 的唯一家;双击 = 打开 Texture Settings)
		TextureSource,
		TextureAsset,
		Script,
		Folder,
		// 兼容别名:P4 之前的代码把"贴图"叫 Texture(= 源图)。新代码请用上面两个名字。
		// **必须放在最后**:枚举值按"上一个枚举项"自增,别名插在中间会把后面的项顶成重复值
		// (实测:插在 Script 之前 → Script 与 TextureAsset 同值 → C2196)。
		Texture = TextureSource,
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
		// Slang-B1:`.slang` 是一等资产(Material Shader)——它写表面函数与参数默认值,
		// `.wmat` 是引用它的材质实例;两者在浏览器里必须有各自的类型名。
		// Slang-B1se(2026-09-23 用户决定:不保留旧扩展名兼容层):只有 `.slang` 是编辑器
		// 资产类型,其余扩展名一律按普通文件显示(本表只描述我们仍然产出的资产)。
		if (extension == ".slang")
			return { EditorAssetKind::Shader, "Material Shader" };
		if (extension == ".wmodel")
			return { EditorAssetKind::Model, "Model" };
		if (extension == ".gltf" || extension == ".glb")
			return { EditorAssetKind::ModelSource, "glTF Source" };
		if (extension == ".wprefab")
			return { EditorAssetKind::Prefab, "Prefab" };
		// M4-TEX P4:内容根的图片是**源图**(材质引用它;产物 `<源图>.wtexc` 由它烘出来);
		// `.wtex` 才是可编辑的纹理**资产**(设置 + `source:`)。
		if (extension == ".wtex")
			return { EditorAssetKind::TextureAsset, "Texture" };
		if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga"
			|| extension == ".bmp")
			return { EditorAssetKind::TextureSource, "Texture Source" };
		if (extension == ".lua" || extension == ".luau")
			return { EditorAssetKind::Script, "Script" };
		return { EditorAssetKind::Unknown, "File" };
	}
}
