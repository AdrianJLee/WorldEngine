#pragma once

#include "World/Core/Export.h"
#include "World/Renderer/MaterialSurface.h"

#include <cstddef>
#include <string>

namespace World
{
	// M4-S3:把一份编译好的表面函数产物装配成"能画的管线"。
	//
	// 键(key)由调用方决定,运行时不解释:
	//  - 已保存的材质着色器:`<内容根相对路径>`,例如 "shaders/glass.slang" —— 场景与预览共用;
	//  - 编辑器里**未保存**的实时改动:`<内容根相对路径>#preview` —— 只有该面板的预览材质用它。
	// 这条键约定是"D2:没保存的编辑绝不进主场景"的实现手段(2026-09-23 用户拍板)。
	//
	// 变体(由产物里的顶点入口决定,不暴露成参数):
	//  - Solid / Transparent 共用 "VSMain";Instanced 用 "VSMainInstanced";Skinned 用 "VSMainSkinned";
	//    像素阶段统一是产物的 PSMain。
	//
	// 线程与生命周期纪律:
	//  - Install/Uninstall/Shutdown 会调用 RHI(driver 建管线),**必须在渲染线程**调用;
	//    编译可以放工作线程,结果拿回渲染线程再 Install;
	//  - 换管线是原子的:先把本次要用的管线全部建好,再一次性发布;旧管线按
	//    `Renderer::QueueRelease` 延迟释放,不在飞行帧里销毁;
	//  - 失败(后端不支持 / 设备缺失 / 变体建不出来)不改变已发布的版本,返回 false + 可读原因。
	class WLD_API MaterialSurfaceRuntime
	{
	public:
		struct InstallResult
		{
			bool Success = false;
			size_t Pipelines = 0;           // 本次建好的变体管线数
			bool ReplacedExisting = false;  // 之前已有同键版本(探针/日志用)
			std::string Error;              // 失败原因(可读,英文,便于日志与探针断言)
		};

		// 安装/替换一个键的管线。artifact 必须来自 MaterialSurfaceCompiler 的成功结果;
		// 顶点阶段缺失的变体不建管线(该变体退回上一份/引擎默认)。
		static InstallResult Install(const std::string& key, const SurfaceArtifact& artifact);
		// 卸载一个键(面板关闭、资产删除);无此键时返回 false。
		static bool Uninstall(const std::string& key);
		// 该键当前是否有已发布的管线(渲染侧选管线用)。
		static bool HasPipeline(const std::string& key);
		// 该键的发布版本号(每次成功 Install 递增;0 = 从未安装)。
		// 可观测:探针/单测用它断言"改代码确实换过管线"。
		static size_t PublishedVersion(const std::string& key);
		// 已安装键数 / 累计发布次数(诊断用)。
		static size_t InstalledKeyCount();
		static size_t PublishCount();
		// 关闭设备前清空(旧管线仍走 QueueRelease)。
		static void Shutdown();
	};
}
