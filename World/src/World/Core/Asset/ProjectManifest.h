#pragma once

#include "World/Core/Export.h"

#include <filesystem>
#include <string>
#include <vector>

namespace World::Asset
{
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
