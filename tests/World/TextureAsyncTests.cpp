// T5c(2026-10-05):纹理异步解码 + 唯一 GPU 驻留。
//
// 验收口径:
//   1. RequestAsync 幂等:重复请求不重排(在飞计数稳定);
//   2. **无设备**(headless)时:PumpCompletions 安全返回 0,不崩、不误清在飞状态;
//   3. 取消/清空后工作线程的结果被丢弃(PendingCount 归零);
//   4. TextureLibrary 是**唯一** GPU 纹理驻留:同一路径 + 同一 srgb 只对应一个条目;
//      srgb 不同则分开(不串味);
//   5. 线程安全契约:RequestAsync 可在主线程调用、Publish 在工作线程写、PumpCompletions 主线程读。
#include "World/Core/StringPool.h"
#include "World/Renderer/Texture/TextureLibrary.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace
{
	using World::PathId;
	using World::StringPool;
	using World::TextureLibrary;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	// 1/3. 幂等 + 清理:无设备环境下全程不崩(README 契约:headless 不碰 RHI)。
	void AsyncIsIdempotentAndSafeWithoutDevice()
	{
		TextureLibrary& library = TextureLibrary::Get();
		library.Clear();

		const PathId path = StringPool::Get().InternPath("textures/__async_probe__.wtex");
		CHECK(!path.IsValid() == false);

		library.RequestAsync(path, /*srgb*/ true);
		const std::size_t first = library.PendingCount();
		// 第二次请求:幂等(不重排)⇒ 在飞数不增加。
		library.RequestAsync(path, /*srgb*/ true);
		CHECK(library.PendingCount() == first);

		// 无设备 ⇒ 提交点安全返回 0(不建纹理、不崩)。
		CHECK(library.PumpCompletions() == 0u);

		// Clear 之后记账归零(工作线程的结果会被 Publish 丢弃)。
		library.Clear();
		CHECK(library.PendingCount() == 0u);

		// 空路径 / 空串请求:安全 no-op。
		library.RequestAsync(PathId(), true);
		library.RequestAsync(std::string(), false);
		CHECK(library.PendingCount() == 0u);
	}

	// 4. srgb 两种用途分开记账(不串味)——无设备下用 PendingCount 观察键空间。
	void SrgbKeysAreDistinct()
	{
		TextureLibrary& library = TextureLibrary::Get();
		library.Clear();
		const PathId path = StringPool::Get().InternPath("textures/__async_srgb__.wtex");
		library.RequestAsync(path, /*srgb*/ true);
		library.RequestAsync(path, /*srgb*/ false);
		// 同一路径两种色彩空间 = 两个独立条目(与收口前的 (path,srgb) 键口径一致)。
		CHECK(library.PendingCount() == 2u);
		library.Clear();
		CHECK(library.PendingCount() == 0u);
	}

	// 5. 诊断字符串稳定可读(加载界面/探针依赖它)。
	void DescribeIsStable()
	{
		TextureLibrary& library = TextureLibrary::Get();
		library.Clear();
		const std::string empty = library.DescribeLoads();
		CHECK(empty.find("pending=0") != std::string::npos);
		const PathId path = StringPool::Get().InternPath("textures/__async_desc__.wtex");
		library.RequestAsync(path, true);
		const std::string busy = library.DescribeLoads();
		CHECK(busy.find("pending=") != std::string::npos);
		library.Clear();
	}
}

int main()
{
	try
	{
		AsyncIsIdempotentAndSafeWithoutDevice();
		SrgbKeysAreDistinct();
		DescribeIsStable();
		std::printf("World.TextureAsync: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.TextureAsync: FAILED: %s\n", error.what());
		return 1;
	}
}
