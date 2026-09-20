// P4-UX16:"新建资产"类型注册表(World/Core/Asset/AssetTypeRegistry)headless 单测。
//
// 这条单测对应验收口径的最后一句:「注册一个假的测试类型 → 菜单里自动多一项且能创建;
// 删掉注册 → 菜单项消失(**零 UI 改动**)」。菜单本身在 Editor 里,这里断言菜单**数据源**:
//   1. 注册 / 覆盖(同 Id)/ 反注册 / 空 Id 忽略 / Term 缺省 = Label;
//   2. Sorted():文件夹恒第一 → SortOrder 升序 → Id 字典序(确定性,与注册顺序无关);
//   3. Create 回调真的被调用、真的落盘(在临时目录里),失败时 error 可读;
//   4. 注册表是进程内单例:同一个 Get() 实例(Editor 与 World 侧看到同一张表)。
#include "World/Core/Asset/AssetTypeRegistry.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using World::AssetTypeDesc;
	using World::AssetTypeRegistry;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	AssetTypeDesc MakeDesc(const std::string& id, const std::string& label, int order, bool folder = false)
	{
		AssetTypeDesc desc;
		desc.Id = id;
		desc.Label = label;
		desc.SortOrder = order;
		desc.IsFolder = folder;
		return desc;
	}

	std::vector<std::string> Ids(const std::vector<AssetTypeDesc>& types)
	{
		std::vector<std::string> ids;
		ids.reserve(types.size());
		for (const AssetTypeDesc& desc : types)
			ids.push_back(desc.Id);
		return ids;
	}
}

int main()
{
	try
	{
		AssetTypeRegistry& registry = AssetTypeRegistry::Get();
		// 单例:两次取到同一个对象(Editor 与 World 共享同一张表的前提)。
		CHECK(&registry == &AssetTypeRegistry::Get());

		// 从干净状态开始(本进程独占注册表)。
		registry.Clear();
		CHECK(registry.Size() == 0);
		CHECK(registry.Sorted().empty());

		// ---- 1. 注册 / 查 / 覆盖 / 反注册 ----
		registry.Register(MakeDesc("scene", "Scene", 20));
		registry.Register(MakeDesc("folder", "Folder", 0, true));
		registry.Register(MakeDesc("material", "Material", 10));
		CHECK(registry.Size() == 3);
		CHECK(registry.Find("material") != nullptr);
		CHECK(registry.Find("material")->Label == "Material");
		CHECK(registry.Find("missing") == nullptr);

		// Term 缺省 = Label(语言切换/歧义对照时用它)。
		CHECK(registry.Find("scene")->Term == "Scene");

		// 同 Id 二次注册 = 覆盖,不是并行两条(菜单里一个类型只能出现一次)。
		registry.Register(MakeDesc("material", "Material (v2)", 10));
		CHECK(registry.Size() == 3);
		CHECK(registry.Find("material")->Label == "Material (v2)");

		// 空 Id 直接忽略(不产生"点不动的幽灵项")。
		registry.Register(MakeDesc("", "Nameless", 5));
		CHECK(registry.Size() == 3);

		CHECK(registry.Unregister("material"));
		CHECK(!registry.Unregister("material"));
		CHECK(registry.Size() == 2);

		// ---- 2. Sorted():文件夹恒第一 → SortOrder → Id ----
		registry.Clear();
		// 故意乱序注册:Id 字典序要把 "alpha" 排在 "beta" 前面(同 SortOrder = 100)。
		registry.Register(MakeDesc("beta", "Beta", 100));
		registry.Register(MakeDesc("alpha", "Alpha", 100));
		registry.Register(MakeDesc("folder", "Folder", 0, true));
		registry.Register(MakeDesc("scene", "Scene", 20));
		registry.Register(MakeDesc("script", "Script", 20));   // 同序 → 按 Id:"scene" < "script"
		{
			const std::vector<AssetTypeDesc> sorted = registry.Sorted();
			const std::vector<std::string> ids = Ids(sorted);
			const std::vector<std::string> expected { "folder", "scene", "script", "alpha", "beta" };
			CHECK(ids == expected);
			// All() 保持注册顺序(消费方要"原样"时用),与 Sorted() 不是同一份。
			CHECK(Ids(registry.All()) != expected);
		}

		// ---- 3. Create 回调:真的被调用、真的落盘、失败有可读原因 ----
		const std::filesystem::path tempDir =
			std::filesystem::temp_directory_path() / "we-asset-type-registry-test";
		std::error_code ignored;
		std::filesystem::remove_all(tempDir, ignored);
		std::filesystem::create_directories(tempDir, ignored);
		CHECK(std::filesystem::is_directory(tempDir));

		{
			AssetTypeDesc probe = MakeDesc("probe", "Probe", 200);
			probe.Extension = ".probe";
			probe.Create = [](const std::filesystem::path& dir, std::string* error)
			{
				std::ofstream out(dir / "probe.probe", std::ios::binary | std::ios::trunc);
				if (!out.is_open())
				{
					if (error) *error = "could not open probe file";
					return false;
				}
				out << "probe";
				return true;
			};
			registry.Register(std::move(probe));
			const AssetTypeDesc* registered = registry.Find("probe");
			CHECK(registered != nullptr);
			CHECK(registered->Extension == ".probe");
			CHECK(registered->Create != nullptr);

			std::string error;
			CHECK(registered->Create(tempDir, &error));
			CHECK(error.empty());
			CHECK(std::filesystem::is_regular_file(tempDir / "probe.probe"));
			CHECK(std::filesystem::file_size(tempDir / "probe.probe") == 5);
		}

		// 失败路径:error 必须带原因(菜单/快捷键据此给状态栏提示)。
		{
			AssetTypeDesc failing = MakeDesc("failing", "Failing", 201);
			failing.Create = [](const std::filesystem::path&, std::string* error)
			{
				if (error) *error = "disk is full";
				return false;
			};
			registry.Register(std::move(failing));
			std::string error;
			CHECK(!registry.Find("failing")->Create(tempDir, &error));
			CHECK(error == "disk is full");
		}

		// 反注册之后菜单数据源里就不再出现这个 Id(验收:删掉注册 → 菜单项消失)。
		CHECK(registry.Unregister("probe"));
		CHECK(registry.Find("probe") == nullptr);
		for (const AssetTypeDesc& desc : registry.Sorted())
			CHECK(desc.Id != "probe");

		registry.Clear();
		CHECK(registry.Size() == 0);
		std::filesystem::remove_all(tempDir, ignored);

		std::printf("World.AssetTypeRegistry: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.AssetTypeRegistry: FAILED: %s\n", error.what());
		return 1;
	}
}
