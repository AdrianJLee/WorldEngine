#pragma once

#include "World/Core/Export.h"

#include <cstddef>
#include <cstdint>

namespace World
{
	class Allocator;
}

namespace World::Profiling
{
	// =============================================================================
	// MemoryTrack —— 内存归因(三段式,见知识库 contract.memory-attribution)
	//
	//   Tag (子系统)  : 线程局部标签作用域,零分配 —— 回答"涨在场景/资产/渲染/脚本?"
	//   Owner(资源)   : RHI 资源显式登记字节   —— 回答"是哪些纹理/缓冲占的?"
	//   Site (调用点) : 采样 + 离屏符号化       —— 候选增强,不在 M2 必做范围
	//
	// 三条硬规则(违反即失真):
	//   R1 **推动式,不是拉取式**:峰值与趋势由引擎每帧驱动(Tick()),
	//      不允许"UI 打开才更新"(旧 MemoryTracker 就是这样,面板不开就没有峰值)。
	//   R2 采集路径**零堆分配、零锁持有跨调用**:表在 Init 时一次性分配,之后只做槽位读写。
	//   R3 标签名必须是**静态字面量**(与 TraceEvents 的 R2 同规则);不受控字符串会悬垂。
	//
	// 全局 operator new/delete 的覆盖边界(必须知道的事):
	//   替换式 operator new 是**按模块**生效的。hook 实现在 GlobalAllocHooks.cpp,由各模块
	//   各自编译一份,但状态统一放在本类(经 WorldRuntime.dll 导出)⇒ 跨模块可见。
	//   没有装 hook 的模块的分配/释放**不进入统计**;这会让"活跃字节"偏大(释放没被看到),
	//   偏大是安全的(不至于错杀),但读数字时要记得这个口径。
	// =============================================================================
	class WLD_API MemoryTrack
	{
	public:
		static constexpr uint32_t kMaxTags = 256;
		static constexpr uint32_t kMaxGpuOwners = 64;
		static constexpr uint32_t kMaxAllocators = 64;
		static constexpr uint32_t kMaxTrendFrames = 3600;

		// ---- 分配器(池)级:轮询式,修复"峰值只在 UI 拉取时更新" ----
		struct AllocatorStat
		{
			const char* Name = nullptr;
			size_t UsedBytes = 0;
			size_t TotalReserved = 0;
			size_t NumAllocations = 0;
			uint64_t PeakBytes = 0;   // 引擎每帧驱动的峰值
		};

		// ---- 标签级:事件式(全局堆) ----
		struct TagStat
		{
			const char* Name = nullptr;
			uint64_t LiveBytes = 0;
			uint64_t PeakBytes = 0;      // 每帧驱动的峰值
			uint64_t LiveCount = 0;
			uint64_t TotalAllocs = 0;
			uint64_t TotalFrees = 0;
		};

		// ---- GPU 驻留(显存) ----
		struct GpuOwnerStat
		{
			const char* Name = nullptr;
			uint64_t Bytes = 0;
			uint64_t LiveCount = 0;
		};

		// 调用点(Site)聚合:同一处分配点的次数与字节。
		struct SiteStat
		{
			void* Origin = nullptr;      // 采样到的返回地址
			uint64_t LiveBytes = 0;
			uint64_t LiveCount = 0;
			uint64_t TotalAllocs = 0;
		};

		struct LeakEntry
		{
			const char* Tag = nullptr;
			void* Address = nullptr;
			uint64_t Size = 0;
			void* Origin = nullptr;   // 采集时采样到的调用点(未采样为 nullptr)
		};

		static MemoryTrack& Get();

		// ---- 生命周期 ----
		static void Init();
		static void Shutdown();
		static bool Enabled() noexcept;

		// ---- 标签作用域(零分配) ----
		// 名字必须是静态字面量(见 R3)。空指针 = 清除标签。
		static void PushTag(const char* tag) noexcept;
		static void PopTag() noexcept;
		static const char* CurrentTagName() noexcept;

		// ---- 分配器登记(单一注册点 = Allocator 基类构造/析构) ----
		static void RegisterAllocator(Allocator* allocator) noexcept;
		static void UnregisterAllocator(Allocator* allocator) noexcept;

		// ---- 全局堆(operator new/delete 钩子调用) ----
		// origin 由 hook 在其**自己的帧**里用 _ReturnAddress() 取 —— 只有在那一帧
		// 取到的才是真正的调用点(在本函数里取会退化成 RecordAllocate 自己)。
		static void RecordAllocate(void* ptr, size_t size, void* origin = nullptr) noexcept;
		static void RecordDeallocate(void* ptr) noexcept;

		// ---- GPU 驻留(显式登记) ----
		static void AddGpuResident(const char* owner, uint64_t bytes) noexcept;
		static void RemoveGpuResident(const char* owner, uint64_t bytes) noexcept;

		// ---- 每帧一次(由 Telemetry::EndFrame 驱动;修 R1 的拉取式缺陷) ----
		static void Tick();

		// ---- 查询(调用方提供缓冲,返回实际条数;不分配) ----
		static size_t CollectAllocators(AllocatorStat* out, size_t capacity);
		static size_t CollectTags(TagStat* out, size_t capacity);
		static size_t CollectGpuOwners(GpuOwnerStat* out, size_t capacity);
		static size_t CollectLeaks(LeakEntry* out, size_t capacity);
		// 站点聚合:按 **活跃字节** 降序(容量不足时保留最大的那些)。
		// 采样:只有被采样到的分配带 Origin(HasOrigin=true 的比例见 SiteSamplingRate)。
		static size_t CollectSites(SiteStat* out, size_t capacity);
		static uint32_t SiteSamplingRate() noexcept;   // N 表示 1/N 采样;0 = 关闭
		// 转储到文件(人读 + 可 diff):Top 站点 + 泄漏明细。返回写入的行数。
		static size_t DumpReport(const char* path, size_t topSites = 24);
		static size_t CollectTrend(uint64_t* liveBytes, uint64_t* gpuBytes, size_t capacity);

		static uint64_t HeapLiveBytes();
		static uint64_t HeapPeakBytes();
		static uint64_t PoolUsedBytes();
		static uint64_t GpuResidentBytes();
		static uint64_t LiveAllocationCount();
		// 表里找不到记录的释放次数(>0 = 有模块没装 hook,或表太小 ⇒ 归因会偏大)。
		static uint64_t UnknownFrees();

		// 文字摘要(人读;与 JSON 查询同源)。
		static std::string DescribeText();
		static std::string DescribeJson();

		// 标签作用域(RAII)。零分配:PushTag 只在首次见到该标签名时做一次扫描。
		// 也可由 WLD_MEM_TAG("...") 宏构造。
		class MemoryTagScope
		{
		public:
			explicit MemoryTagScope(const char* tag) noexcept
			{
				MemoryTrack::PushTag(tag);
			}
			~MemoryTagScope()
			{
				MemoryTrack::PopTag();
			}
			MemoryTagScope(const MemoryTagScope&) = delete;
			MemoryTagScope& operator=(const MemoryTagScope&) = delete;
		};

	private:
		MemoryTrack() = default;
		MemoryTrack(const MemoryTrack&) = delete;
		MemoryTrack& operator=(const MemoryTrack&) = delete;

		uint32_t AcquireTag(const char* name) noexcept;   // 0 = 未标记
		void ReleaseTag(uint32_t tagId) noexcept;

		void InsertRecord(void* ptr, size_t size, uint32_t tagId, void* origin, uint32_t siteIndex) noexcept;
		// 站点表项:已存在则复用,否则新建。返回下标(表满返回 kNoSite)。
		uint32_t AcquireSite(void* origin) noexcept;
		bool EraseRecord(void* ptr, size_t* sizeOut, uint32_t* tagOut, uint32_t* siteOut) noexcept;
	};
}
