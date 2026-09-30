#pragma once

#include "World/Renderer/TextureArtifact.h"
#include "World/Renderer/TextureImportSettings.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	namespace Editor
	{
		// HOTR-P2-T5:纹理产物(`.wtexc`)烘焙 —— 从 `TextureSettingsPanel.cpp` 的匿名命名空间**提取**。
		//
		// 为什么要独立文件:面板的 Apply / Reimport / Reset(主线程、用户点了才烘)与编辑器级
		// `TextureImportWatch`(内容根下 `.wtex`/源图被外部改动 → 自动重烘)必须走**同一份**口径 ——
		// 字节来源(容器内嵌 payload / 旧式外部源图)、产物命名(契约名 `<同目录>/<主名>.wtexc`)、
		// 原子写盘、材质贴图缓存失效(资产引用 + 源图引用两条键)。提取后面板只调用本文件,行为逐字节不变。
		//
		// 线程纪律(与 TextureSettingsPanel 的后台预览烘焙同一套口径):
		//   - 编码(`TextureCompiler::BakeBytes/BakeFile`)是纯 CPU,**可在工作线程**跑;
		//   - 写盘 + `MaterialTextureCache::Invalidate` + 日志只在**主线程**执行(提交入口)。

		// 一步到位(面板用,主线程):读源字节 → 编码 → 写 `<同目录>/<主名>.wtexc` + 失效材质贴图缓存。
		//
		// `sourceLogical` 是**内容根相对**逻辑路径,两种写法都合法:
		//   * `.wtex` 资产(单文件容器)= 按**内嵌 payload** 烘(与 cook 同一口径,不读外部源图);
		//   * 源图(`.png/.jpg/.jpeg/.tga/.bmp`)= 按该外部文件烘(旧式设置文件走这条)。
		// 失败填 outError(人话);成功时产物落盘并已让材质贴图缓存失效。
		bool BakeTextureArtifactNow(const std::filesystem::path& contentRoot,
			const std::string& sourceLogical, const TextureImportSettings& settings, std::string& outError);

		// 主线程提交(TextureImportWatch 用):把**工作线程**烘好的产物字节写到契约产物名
		// `<byteSourceFile 同目录>/<主名>.wtexc`(原子替换,与 BakeTextureArtifactNow 同一条实现),
		// 清掉同源的旧命名残留副本,并按逻辑路径失效材质贴图缓存(资产引用与源图引用两条键)
		// + 打 `[texture] baked …` 日志。
		//
		// `byteSourceFile` = 产物字节的**来源文件**绝对路径(容器 = `.wtex` 自身;旧式 = 外部源图)
		// —— 契约名与旧命名残留都从它推导,调用方不要自己拼产物名。
		bool CommitTextureArtifact(const std::filesystem::path& contentRoot, const std::string& sourceLogical,
			const std::filesystem::path& byteSourceFile, const std::vector<uint8_t>& artifactBytes,
			const TextureArtifactHeader& header, std::string& outError);
	}
}
