// =============================================================================
// 全局 operator new / delete 钩子(内存归因的 L2 层)
//
// 为什么需要它:引擎里 99.9% 的分配走默认堆(std::vector / std::string / EnTT /
// Luau / Jolt / RHI),只统计 Allocator 子类会得到"内存很稳定"的**假结论**。
//
// 覆盖边界(必须知道):
//   * 替换式 operator new 是**按模块**生效的(MSVC /MD 下每个 DLL/EXE 各自解析)。
//     本文件编译进 World 模块,覆盖引擎自身代码的分配;宿主(Editor/Runtime/Game)
//     若要一并覆盖,把本文件加进对应 target 的源列表即可 —— 状态放在
//     MemoryTrack(经 WorldRuntime.dll 导出),因此各模块共享同一份统计。
//   * 不装 hook 的模块所做的释放不会被看到 ⇒ "活跃字节"只会**偏大**,不会偏小。
//     偏大是安全的(不会错杀),但读数字时要记得这个口径(见 MemoryTrack 的说明)。
//
// 三条纪律(旧 Instrumentor 踩过的坑,别重蹈):
//   1. 钩子里**不做任何分配**(内存表在 Init 时一次性分配)。已分配时可能触发分配。
//   2. 必须递归保护:簿记若再触发 new 会无限递归。
//   3. 只观测,**不改变**内存布局(不打头、不加偏移)——跨模块释放因此不会崩。
// =============================================================================

#include "wldpch.h"
#include "World/Profiling/MemoryTrack.h"

#include <cstdlib>

#if defined(WLD_PLATFORM_WINDOWS)
#include <intrin.h> // _ReturnAddress:调用点采样(非 SIMD)
#endif
#include <new>

namespace
{
	// 递归保护:簿记路径本身绝不能再进入簿记。
	thread_local int t_HookDepth = 0;

	struct HookGuard
	{
		HookGuard() noexcept { ++t_HookDepth; }
		~HookGuard() { --t_HookDepth; }
		bool ShouldRecord() const noexcept { return t_HookDepth == 1; }
	};
}

void* operator new(size_t size)
{
	if (void* memory = std::malloc(size ? size : 1))
	{
		HookGuard guard;
		if (guard.ShouldRecord())
			// 在 hook 自己的帧里取返回地址 —— 这才是用户代码里的分配点
			// (在 MemoryTrack 里取会退化成 RecordAllocate 自己,站点表就废了)。
			World::Profiling::MemoryTrack::RecordAllocate(memory, size, _ReturnAddress());
		return memory;
	}
	throw std::bad_alloc();
}

void* operator new[](size_t size)
{
	return ::operator new(size);
}

void* operator new(size_t size, const std::nothrow_t&) noexcept
{
	void* memory = std::malloc(size ? size : 1);
	if (!memory)
		return nullptr;
	HookGuard guard;
	if (guard.ShouldRecord())
		World::Profiling::MemoryTrack::RecordAllocate(memory, size, _ReturnAddress());
	return memory;
}

void* operator new[](size_t size, const std::nothrow_t& tag) noexcept
{
	return ::operator new(size, tag);
}

void operator delete(void* memory) noexcept
{
	if (!memory)
		return;
	HookGuard guard;
	if (guard.ShouldRecord())
		World::Profiling::MemoryTrack::RecordDeallocate(memory);
	std::free(memory);
}

void operator delete[](void* memory) noexcept
{
	::operator delete(memory);
}

void operator delete(void* memory, size_t) noexcept
{
	::operator delete(memory);
}

void operator delete[](void* memory, size_t) noexcept
{
	::operator delete(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept
{
	::operator delete(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept
{
	::operator delete(memory);
}

// 过对齐版本(C++17):不带簿记的直通实现,避免把对齐路径也拖进统计的复杂度里。
// 它们仍要存在,否则替换了普通版本后过对齐分配会落到 CRT 的默认实现上而行为不一致。
void* operator new(size_t size, std::align_val_t alignment)
{
	const size_t aligned = static_cast<size_t>(alignment);
	const size_t rounded = (size + aligned - 1) & ~(aligned - 1);
	void* memory = _aligned_malloc(rounded ? rounded : aligned, aligned);
	if (!memory)
		throw std::bad_alloc();
	return memory;
}

void* operator new[](size_t size, std::align_val_t alignment)
{
	return ::operator new(size, alignment);
}

void operator delete(void* memory, std::align_val_t alignment) noexcept
{
	if (memory)
		_aligned_free(memory);
}

void operator delete[](void* memory, std::align_val_t alignment) noexcept
{
	::operator delete(memory, alignment);
}

void operator delete(void* memory, size_t, std::align_val_t alignment) noexcept
{
	::operator delete(memory, alignment);
}

void operator delete[](void* memory, size_t, std::align_val_t alignment) noexcept
{
	::operator delete(memory, alignment);
}
