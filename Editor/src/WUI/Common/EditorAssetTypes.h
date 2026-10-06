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
		// CPPSRC-1(用户 2026-09-29「c++脚本要像 asset 资产一样在编辑器里展示」):项目层 C++
		// 源码在内容浏览器里的两个一等类型。它们**不是可引用资产**(AssetCatalog::MatchesKind
		// 的 default 分支返回 false),只是"像资产一样"展示:同一套网格/列表、类型列、徽标、
		// 双击打开(外部 Visual Studio)。源码根 = `<项目根>/src`(内容根之外,见
		// ContentBrowserPanel 的双根口径)。
		CppHeader,      // .h / .hpp / .inl
		CppSource,      // .c / .cc / .cpp / .cxx
		UiDocument,     // .wui
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
		// PECS-T11:类型列的本地化 key(非空时优先于 Kind 的固定 key)。目前只有 Lua 按**目录**
		// 分流(systems/ → Lua System,lib/ → Lua Library,其余 → Lua Script),所以名字与 key
		// 由 DescribeAssetType 一起给出;其它类型保持 nullptr,由内容浏览器按 Kind 映射。
		const char* LocalizationKey = nullptr;
	};

	inline std::string LowerExtension(const std::filesystem::path& path)
	{
		std::string extension = path.extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return extension;
	}

	// PECS-T11:`.luau`/`.lua` 的类型列按**目录**给名 —— 与「新建 Lua …」向导的三类对齐:
	// `<内容根>/scripts/systems/**` → Lua System(Play 自动装载),
	// `<内容根>/scripts/lib/**`     → Lua Library(不自动加载),
	// 其余                          → Lua Script。
	// 只认"`…/scripts/<systems|lib>/<file>`"这个最后三段形态,内容根以外的同名目录不会误判。
	// 返回:0 = 普通脚本,1 = systems/,2 = lib/。
	inline int LuaScriptFlavor(const std::filesystem::path& path)
	{
		auto iterator = path.end();
		if (iterator == path.begin())
			return 0;
		--iterator;                          // 文件名
		if (iterator == path.begin())
			return 0;
		--iterator;                          // 直接父目录(systems / lib / …)
		const std::string parent = iterator->string();
		if (iterator == path.begin())
			return 0;
		--iterator;                          // 祖父目录,必须恰好是 scripts
		if (iterator->string() != "scripts")
			return 0;
		if (parent == "systems")
			return 1;
		if (parent == "lib")
			return 2;
		return 0;
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
		// M7a(GameUI):`.wui` 是 GameUI 的**设计数据**资产(单一扩展名,与其他核心资产同款);
		// 类型名走 Wui::Tr("asset.file.ui", "UI Document"),这里的 Name 只作英文兜底/日志用。
		if (extension == ".wui")
			return { EditorAssetKind::UiDocument, "UI Document" };
		// M4-TEX P4:内容根的图片是**源图**(材质引用它;产物 `<源图>.wtexc` 由它烘出来);
		// `.wtex` 才是可编辑的纹理**资产**(设置 + `source:`)。
		if (extension == ".wtex")
			return { EditorAssetKind::TextureAsset, "Texture" };
		if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga"
			|| extension == ".bmp")
			return { EditorAssetKind::TextureSource, "Texture Source" };
		if (extension == ".lua" || extension == ".luau")
		{
			// PECS-T11:同一扩展名按目录分三种显示名(与向导的 系统 / 脚本库 / 空 对齐)。
			const int flavor = LuaScriptFlavor(path);
			if (flavor == 1)
				return { EditorAssetKind::Script, "Lua System", "asset.file.lua_system" };
			if (flavor == 2)
				return { EditorAssetKind::Script, "Lua Library", "asset.file.lua_library" };
			return { EditorAssetKind::Script, "Lua Script", "asset.file.lua_script" };
		}
		// CPPSRC-1:项目层 C++ 源码(类型名走 Wui::Tr("asset.file.cpp_header" / "asset.file.cpp_source"),
		// 这里的 Name 只作英文兜底/日志用)。
		if (extension == ".h" || extension == ".hpp" || extension == ".inl")
			return { EditorAssetKind::CppHeader, "C++ Header" };
		if (extension == ".c" || extension == ".cc" || extension == ".cpp" || extension == ".cxx")
			return { EditorAssetKind::CppSource, "C++ Source" };
		return { EditorAssetKind::Unknown, "File" };
	}

	// CPPSRC-1:`src/Generated/**` 是引擎/项目构建生成物(schema 注册 + `Game.manifest` 同步),
	// 在内容浏览器里照常列出(用户口径"要看得见"),但徽标与类型列要标成"Generated",
	// 让"能改的源码"和"别手改的生成物"一眼分开。相对路径的第一段是 `Generated` 即命中。
	inline bool IsGeneratedSourcePath(const std::filesystem::path& relativeToSourceRoot)
	{
		auto it = relativeToSourceRoot.begin();
		if (it == relativeToSourceRoot.end())
			return false;
		std::string first = it->string();
		std::transform(first.begin(), first.end(), first.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return first == "generated";
	}
}
