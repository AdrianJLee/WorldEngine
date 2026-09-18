#pragma once

#include "World/Core/Export.h"

#include <filesystem>
#include <string>
#include <vector>

namespace World::Asset
{
	// 项目级渲染设置(project.we.yaml 的 `rendering:` 区块)。
	//
	// 为什么放在清单里:这些开关/上限过去只藏在代码常量或环境变量里,引擎用户
	// (做游戏的人)无法在不改引擎、不设环境变量的前提下调整。清单是项目的单一
	// 事实源,编辑器里也有面板可改(见 Editor 的 settings 面板)。
	//
	// 约束(ValidateManifest 会拒绝越界值):
	//  - ShadowMapSize:2 的幂,256..4096;
	//  - MaxDirectionalLights:1..2;MaxPointLights:0..7;两者之和 ≤ 8(UBO 容量)。
	struct RenderingSettings
	{
		bool Culling = true;              // 视锥剔除(主通道按相机、阴影通道按光源)
		bool Shadows = true;              // 方向光阴影通道
		uint32_t ShadowMapSize = 2048;    // 阴影贴图边长(启动时生效)
		uint32_t MaxDirectionalLights = 1;
		uint32_t MaxPointLights = 7;
	};

	// 项目清单(project.we.yaml):资产内容根、启动场景与发行包列表的单一事实源。
	// ContentRoot 相对 manifest 文件所在目录;Packages 为发行布局下的相对路径。
	class WLD_API ProjectManifest
	{
	public:
		std::string Id;
		std::string Version = "1.0.0";
		std::filesystem::path ContentRoot = "assets";
		std::string StartScene;
		std::string Renderer = "opengl";   // opengl | vulkan
		std::vector<std::string> Packages;
		// D8a2:渲染设置(缺省 = 引擎默认;`rendering:` 区块缺失时保持默认)。
		RenderingSettings Rendering;

		// 加载并校验;error 为空表示成功。
		static bool Load(const std::filesystem::path& path, ProjectManifest* out, std::string* error);
		// 序列化(ContentRoot 始终写相对路径,与清单文件目录无关)。
		static bool Save(const std::filesystem::path& path, const ProjectManifest& manifest, std::string* error);

		// 相对 manifest 文件目录解析内容根绝对路径。
		std::filesystem::path ResolveContentRoot(const std::filesystem::path& manifestPath) const;
		// 按工作目录依次尝试 project.we.yaml、Game/project.we.yaml;命中则 out 填实际路径。
		static bool Locate(const std::filesystem::path& workingDirectory, std::filesystem::path* manifestPath);
	};
}
