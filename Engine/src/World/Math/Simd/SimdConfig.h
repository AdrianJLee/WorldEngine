#pragma once

// SIMD 配置与架构门(标准 docs/dev/performance-and-data-layout.md §6.5)。
//
// 这是**唯一**允许手写 intrinsics 的目录(S1 集中化 / 门禁 E)。规则要点:
//   * S2:每个用到 intrinsics 的文件显式包含本头,再由它包含架构头 —— 不依赖传递包含;
//   * S3:每个 SIMD 路径必须有等价标量回退,且两条路都被测试;
//   * S4:逐 lane 与标量**同序**的写法可默认开启(逐位一致);改变运算顺序/引入 FMA
//     收缩/开 FTZ 的写法必须门控并默认关 —— 引擎对 P4/P5/P6/P7 有逐位可复现承诺;
//   * S5:禁止在业务代码里临时开 `/arch:*`、`GLM_FORCE_ALIGNED_GENTYPES` 之类全局开关。

// ---- 架构检测 ----
// 目前只有 Windows/x64 目标;这里把"有没有 SSE2"做成显式宏,而不是隐含假设 ——
// 这样将来加 ARM64 时,回退路径是**已经存在并被测试过**的,不是暗雷。
#if defined(_M_X64) || defined(__x86_64__)
#	define WLD_SIMD_X86_64 1
#else
#	define WLD_SIMD_X86_64 0
#endif

// x64 的 ABI 保证 SSE2 可用(无需编译开关);x86-32 需要 /arch:SSE2。
#if WLD_SIMD_X86_64
#	define WLD_SIMD_SSE2 1
#	define WLD_SIMD_LANES 4
#	include <immintrin.h>   // S2:显式包含,不靠传递包含
#else
#	define WLD_SIMD_SSE2 0
#	define WLD_SIMD_LANES 1
#endif

namespace World::Math::Simd
{
	// 运行时自报:便于日志/测试断言"这条路径是否真的走了 SIMD"。
	inline const char* BackendName()
	{
#if WLD_SIMD_SSE2
		return "sse2";
#else
		return "scalar";
#endif
	}
}
