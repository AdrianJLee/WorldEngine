#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace World::Editor
{
	// 打包(烘焙 + 发行布局)的单一实现:编辑器 UI 的 Cook 按钮与
	// `Editor.exe --cook <dir>` 无窗口模式走同一段代码。
	struct CookOptions
	{
		std::filesystem::path PublishDir;          // 发行目录(会被创建;CheckOnly 时不创建)
		std::filesystem::path StartSceneOverride;  // 可选:覆盖清单里的启动场景(相对内容根)
		bool CheckOnly = false;                    // W7-2:只跑资产导入(含脚本编译门),跳过发行步骤
		// M4-TEX P3:发行包剥离纹理源图(默认关)。开启时打包前删掉 cooked 里内容根映射过来的
		// `.png/.jpg/.jpeg/.tga/.bmp` 与 `.wtex` sidecar,只留 `.wtexc` 产物。
		bool StripSourceTextures = false;
		// WLD-L10N-S2:发行包只带选中的语言(空 = 扫描 engine/project 两层目录得到的全部语言)。
		// 语言 = 层目录下的语言子目录名(`zh-CN`、`en`…),不写死清单;编辑器层从不进游戏包。
		std::vector<std::string> Languages;
	};

	struct CookResult
	{
		bool Ok = false;
		std::string Error;
		size_t AssetsTotal = 0;
		size_t AssetsChanged = 0;
		size_t AssetsSkipped = 0;
		size_t ShaderArtifacts = 0;         // 全部着色器产物的总数(老口径 + 双目标 + 表面材质)
		size_t DistributionArtifacts = 0;   // Slang-T5:引擎着色器的双目标 SPIR-V(.spv + .gl.spv)
		size_t SurfaceShaders = 0;          // Slang-T5:烘焙过的表面材质着色器(`.slang`)数
		size_t SurfaceArtifacts = 0;        // Slang-T5:表面材质的 SPIR-V + 反射 JSON 产物数
		size_t LocalizationLanguages = 0;   // WLD-L10N-S2:发行包实际带上的语言数(engine ∪ project)
		size_t LocalizationFiles = 0;       // WLD-L10N-S2:拷贝的语言包文件数(域文件 + catalog.json)
		size_t TextureBaked = 0;            // M4-TEX P3:本次真烘的贴图数(缓存未命中)
		size_t TextureUpToDate = 0;         // M4-TEX P3:命中纹理缓存(源与设置都没变)的张数
		size_t TextureSkipped = 0;          // M4-TEX P3:源图空/读不了而跳过的张数
		size_t StrippedSourceTextures = 0;  // M4-TEX P3:剥离掉的源图 + sidecar 文件数(未开开关 = 0)
		size_t RestoredSourceCopies = 0;    // M4-TEX P3:剥离后自愈补回的源图 + sidecar 文件数(通常 0)
	};

	// 执行一次完整打包:项目清单 → 增量烘焙 → 着色器烘焙 → 内容包 →
	// Runtime.exe/WorldRuntime.dll/Game.dll → 发行清单。
	CookResult CookProject(const CookOptions& options);
}
