#pragma once

#include "World/Core/Export.h"
#include "World/Renderer/MaterialParams.h"
#include "World/Renderer/MaterialSurfaceContract.hlsli"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace World
{
	// M4-S1:用户表面函数的编译目标。
	// Slang-T3:两家目标都由 slangc 出 SPIR-V(同一源码、同一编译器、每家一份 permutation);
	//  - VulkanSpirV:Vulkan profile(产物 SPIR-V 1.3);
	//  - OpenGLSpirV:`<stage>_5_0+spirv_1_0` —— ARB_gl_spirv 只接受 SPIR-V 1.0 与组合采样器,
	//    GL 侧的运行时接入(M4-S4)另立项,本层只负责编译出目标产物。
	enum class SurfaceShaderBackend : uint8_t
	{
		VulkanSpirV = 0,
		OpenGLSpirV = 1,
	};

	// 编译器(Slang)的一条诊断。用户源里的错误会被映射回**用户源文件的行列号**
	// (InUserSource = true, UserLine/UserColumn 有效);包装模板自身的错误只有 File/Line/Column。
	struct WLD_API SurfaceDiagnostic
	{
		std::string Severity;   // "error" | "warning"
		std::string Message;    // 编译器原始整行(含 E 码,如 error[E20002]: …)
		std::string File;       // 编译器报告的文件;用户源错误时是 surface_user.slang(生成的中间源)
		uint32_t Line = 0;      // 报告文件内的行
		uint32_t Column = 0;    // 报告文件内的列
		bool InUserSource = false;
		uint32_t UserLine = 0;   // 映射回用户源的行(InUserSource 时有效)
		uint32_t UserColumn = 0; // 映射回用户源的列(InUserSource 时有效)
	};

	// 可缓存的编译产物。第一版只有 SPIR-V(Vulkan);OpenGL 由 M4-S4 补。
	// M4-S3:表面模板自带的顶点阶段(按入口名区分)。
	//
	// 为什么不能复用引擎 Renderer3D_Solid.slang 的 VS:表面模板的 VS 输出是 4 个插值量
	// (loc0 法线 / loc1 世界位置 / loc2 UV / loc3 实体 id),引擎的是 5 个(loc3 是基础色、
	// loc4 才是实体 id)—— 混用在 Vulkan 上属于**接口不匹配**,不是风格问题。
	// 变体与入口的对应:solid/transparent = "VSMain"、instanced = "VSMainInstanced"、
	// skinned = "VSMainSkinned"。
	struct WLD_API SurfaceVertexStage
	{
		std::string EntryPoint;   // "VSMain" | "VSMainInstanced" | "VSMainSkinned"
		std::vector<uint8_t> Bytecode;
	};

	struct WLD_API SurfaceArtifact
	{
		std::vector<uint8_t> Bytecode;
		std::string CacheKey;        // 16 进制内容键(源 + 后端 + 排列键 + 工具/契约身份)
		std::string Backend;         // BackendName(backend)
		std::string EntryPoint;      // 当前固定 "PSMain"
		uint64_t SourceHash = 0;     // 用户源内容哈希(不含包装模板)
		// M4-S3:顶点阶段(见 SurfaceVertexStage)。它们不引用用户的 Evaluate(),因此按
		// **模板键**缓存(包装模板 + 参数块 + 排列键 + 工具/契约身份,不含用户源):
		// 改代码时只有 PSMain 需要真的跑编译器。
		std::vector<SurfaceVertexStage> VertexStages;

		const SurfaceVertexStage* FindVertexStage(const char* entryPoint) const
		{
			if (!entryPoint)
				return nullptr;
			for (const SurfaceVertexStage& stage : VertexStages)
			{
				if (stage.EntryPoint == entryPoint)
					return &stage;
			}
			return nullptr;
		}

		size_t ByteSize() const { return Bytecode.size(); }
	};

	// 编译结果。失败时**不抛异常**,细节在 Diagnostics / RawToolOutput 里。
	// 注意:失败不会自动回退到 LastGood();是否继续用上一份管线由调用方决定,
	// 通过 MaterialSurfaceCompiler::LastGood() 查询。
	struct WLD_API SurfaceCompileResult
	{
		bool Success = false;
		bool CacheHit = false;
		SurfaceArtifact Artifact;
		std::vector<SurfaceDiagnostic> Diagnostics;
		std::string RawToolOutput;
		double ElapsedMilliseconds = 0.0;
		bool LastGoodAvailable = false;
	};

	// M4-S1:表面函数编译入口。
	//
	// 输入是**用户源文本**(函数 `Surface Evaluate(MaterialInputs input)`)与排列键;
	// 引擎把用户源包进固定模板(顶点/光照/阴影/实例化/蒙皮/雾钩子),由 slangc 编成
	// (`-spirv -T ps_6_0 -E PSMain`)编译成 SPIR-V。空源 = 使用引擎默认表面函数(起始代码)。
	//
	// 缓存:键 = 包装后源内容(含契约与用户源)+ 后端 + 排列键 + 工具/契约身份;
	// 命中直接读缓存,不调用工具。命中/未命中/工具调用都有计数器。
	class WLD_API MaterialSurfaceCompiler
	{
	public:
		static SurfaceCompileResult CompileSurface(const std::string& source, const std::string& permutationKey,
			SurfaceShaderBackend backend = SurfaceShaderBackend::VulkanSpirV);

		// M4-S2:带注解参数表的编译。参数块(`cbuffer MaterialParams` + 贴图槽)由 table 生成,
		// 插在引擎模板之后、用户源之前 —— 注解是参数的事实源,用户源里不再手写参数块。
		// CompileSurface(source, key) 等价于先 ParseMaterialParams(source) 再走这里
		// (注解解析失败 → 结构化诊断,不调用工具)。
		static SurfaceCompileResult CompileSurfaceWithParams(const std::string& source,
			const std::vector<MaterialParamDecl>& params, const std::string& permutationKey,
			SurfaceShaderBackend backend = SurfaceShaderBackend::VulkanSpirV);

		// S3 的"新建 `.slang` 起始代码"(legacy `.hlsl` 同源):只含用户可编辑的 Evaluate();默认值来自
		// MakeDefaultSurface(),不会漏字段。
		static std::string DefaultSurfaceFunctionSource();
		// 引擎包装后的完整 Slang 源(契约 + 模板 + 用户源;HLSL 语法是 Slang 的子集)。
		// 契约文件读不到时返回空串;
		// CompileSurface 会把这种情况变成结构化错误而不是断言。
		static std::string WrapSurfaceSource(const std::string& userSource,
			SurfaceShaderBackend backend = SurfaceShaderBackend::VulkanSpirV);
		static std::string WrapSurfaceSourceWithParams(const std::string& userSource,
			const std::vector<MaterialParamDecl>& params,
			SurfaceShaderBackend backend = SurfaceShaderBackend::VulkanSpirV);
		// 参数块源码(注解 → `cbuffer MaterialParams` + Texture2D/SamplerState 声明)。
		// 反射与编译共用同一份生成器,保证"声明 == 编译 == 反射"是同一件事。
		static std::string BuildParamBlockSource(const std::vector<MaterialParamDecl>& params);
		static std::string ContractHeaderPath();

		// Slang-T3:与 artifact 同键的 Slang 反射 JSON(`-reflection-json`)路径。
		// 反射(MaterialParams.cpp)读它;文件不存在(旧缓存 / 工具没写)时返回空串。
		static std::string ReflectionPath(const SurfaceArtifact& artifact);

		// 上一次成功编译的产物查询(按"后端 + 排列键"分别保存)。失败后由调用方决定
		// 是否继续用这份;编译器本身不会静默返回旧产物。
		static bool LastGood(const std::string& permutationKey, SurfaceShaderBackend backend, SurfaceArtifact& out);
		static void ClearLastGood();

		static const char* BackendName(SurfaceShaderBackend backend);

		// 可观测计数(与 ShaderCompiler 的计数分开,避免和烘焙路径互相污染)。
		static size_t CacheHitCount();
		static size_t CacheMissCount();
		static size_t ToolInvocationCount();
		static void ResetCounters();
	};
}
