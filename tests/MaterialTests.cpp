// D3:材质资产(解析/版本/默认值/夹紧/往返)与材质库(缓存/保存/热重载)回归。
#include "World/Renderer/Material.h"
#include "World/Renderer/MaterialLibrary.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	const char* kSample = R"(FormatVersion: 1
Name: "Steel"
BaseColor: [0.8, 0.7, 0.6, 1.0]
Metallic: 1.0
Roughness: 0.25
Emissive: [0.1, 0.2, 0.3]
AlbedoTexture: "textures/quadrants.png"
NormalTexture: ""
BlendMode: Transparent
DoubleSided: true
)";
}

int main()
{
	try
	{
		using namespace World;

		// 1. 正常解析:各字段落到正确位置。
		{
			MaterialDesc desc;
			std::string error;
			CHECK(MaterialIO::Parse(kSample, desc, &error).Success);
			CHECK(desc.Name == "Steel");
			CHECK(desc.BaseColor == glm::vec4(0.8f, 0.7f, 0.6f, 1.0f));
			CHECK(desc.Metallic == 1.0f);
			CHECK(desc.Roughness == 0.25f);
			CHECK(desc.Emissive == glm::vec3(0.1f, 0.2f, 0.3f));
			CHECK(desc.AlbedoTexture == "textures/quadrants.png");
			CHECK(desc.NormalTexture.empty());
			CHECK(desc.BlendMode == MaterialBlendMode::Transparent);
			CHECK(desc.DoubleSided);
			CHECK(error.empty());
		}

		// 2. 版本拒绝:更高版本必须失败(不猜、不降级)。
		{
			MaterialDesc desc;
			std::string error;
			const auto result = MaterialIO::Parse("FormatVersion: 99\nName: \"x\"\n", desc, &error);
			CHECK(!result.Success);
			CHECK(!result.Error.empty());
		}

		// 3. 缺省值:最小文件也能得到确定默认值。
		{
			MaterialDesc desc;
			CHECK(MaterialIO::Parse("FormatVersion: 1\n", desc, nullptr).Success);
			CHECK(desc.BaseColor == glm::vec4(1.0f));
			CHECK(desc.Roughness == 0.5f);
			CHECK(desc.BlendMode == MaterialBlendMode::Opaque);
			CHECK(!desc.DoubleSided);
		}

		// 4. 越界夹紧 + 警告(不失败)。
		{
			MaterialDesc desc;
			std::string error;
			const auto result = MaterialIO::Parse(
				"FormatVersion: 1\nMetallic: 5.0\nRoughness: 0.0\nBlendMode: Weird\n", desc, &error);
			CHECK(result.Success);
			CHECK(desc.Metallic == 1.0f);
			CHECK(desc.Roughness > 0.0f);
			CHECK(desc.BlendMode == MaterialBlendMode::Opaque);
			CHECK(!error.empty());
		}

		// 5. 往返一致:读 → 写 → 读 完全相等。
		{
			MaterialDesc first;
			CHECK(MaterialIO::Parse(kSample, first, nullptr).Success);
			const std::string text = MaterialIO::Serialize(first);
			MaterialDesc second;
			CHECK(MaterialIO::Parse(text, second, nullptr).Success);
			CHECK(first == second);
		}

		// 6. 序列化文本可被再次解析(写出后自身合法)。
		{
			MaterialDesc desc;
			desc.Name = "Quote \"Test\"";
			desc.AlbedoTexture = "textures/a b.png";
			MaterialDesc roundTrip;
			CHECK(MaterialIO::Parse(MaterialIO::Serialize(desc), roundTrip, nullptr).Success);
			CHECK(roundTrip.Name == desc.Name);
			CHECK(roundTrip.AlbedoTexture == desc.AlbedoTexture);
		}

		// 7. 材质库:缓存同一性 / 保存 / 重载 / Revision 变化。
		{
			// 沙箱目录放在 Game/assets 下的临时子目录:MaterialIO 的读写路径约定是
			// "相对 Game/assets",测试与引擎用同一条解析路径才有意义。
			// 前置:只在仓库根运行(否则 ReadFileText 的 Game/ 回退定位不到,测试无意义)。
			CHECK(std::filesystem::exists(std::filesystem::current_path() / "CMakeLists.txt"));
			const std::filesystem::path directory = std::filesystem::current_path() / "Game" / "assets"
				/ "material_tests_tmp";
			std::error_code ec;
			std::filesystem::create_directories(directory, ec);
			const std::string relative = "material_tests_tmp/library_case.wmat";
			const std::filesystem::path relativeFull = std::filesystem::current_path() / "Game" / "assets" / relative;
			{
				std::ofstream file(relativeFull, std::ios::binary | std::ios::trunc);
				file << kSample;
			}

			MaterialLibrary& library = MaterialLibrary::Get();
			std::string error;
			Ref<Material> loaded = library.Load(relative, &error);
			CHECK(loaded != nullptr);
			CHECK(loaded->GetDesc().Name == "Steel");
			CHECK(library.Load(relative, nullptr) == loaded);   // 同一路径 = 同一实例

			// 修改 → Revision 自增(渲染侧缓存据此失效)。
			const uint32_t before = loaded->GetRevision();
			loaded->SetRoughness(0.9f);
			CHECK(loaded->GetRevision() > before);
			CHECK(loaded->IsDirty() == false);   // setter 只改参数与 Revision,不自动标记"未保存"

			// 保存:写盘 + 回读校验 + 路径/脏标记更新。
			loaded->MarkDirty(true);
			CHECK(library.Save(loaded, relative, &error));
			CHECK(!loaded->IsDirty());
			CHECK(loaded->GetPath() == relative);

			MaterialDesc reloaded;
			// 直接读磁盘断言写出的内容(ReadFileText 的路径约定是相对 Game/assets,
			// 这里的临时文件在 build/ 下,所以用 ifstream 读)。
			std::string diskText;
			{
				std::ifstream file(relativeFull, std::ios::binary);
				diskText.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			}
			CHECK(!diskText.empty());
			CHECK(MaterialIO::Parse(diskText, reloaded, nullptr).Success);
			CHECK(reloaded.Roughness > 0.89f && reloaded.Roughness < 0.91f);

			// 重载:原地更新同一实例(编辑器引用不失效)。
			{
				std::ofstream file(relativeFull, std::ios::binary | std::ios::trunc);
				file << MaterialIO::Serialize([] {
					MaterialDesc desc;
					desc.Name = "Steel2";
					return desc;
				}());
			}
			CHECK(library.Reload(relative, &error));
			CHECK(loaded->GetDesc().Name == "Steel2");

			// 另存为:路径更新,缓存指向新键,旧键不再返回该实例。
			const std::string copyPath = "material_tests_tmp/library_copy.wmat";
			CHECK(library.Save(loaded, copyPath, &error));
			CHECK(loaded->GetPath() == copyPath);
			CHECK(library.Load(copyPath, nullptr) == loaded);   // 路径已被规范化为 Game/assets 相对
			// Save As 后旧文件仍在磁盘上(不是删除语义),测试自行清理,免得留下垃圾资产。
			std::filesystem::remove(relativeFull, ec);

			std::filesystem::remove(directory / "library_copy.wmat", ec);
			std::filesystem::remove(directory / "library_copy.wmat.tmp", ec);
			std::filesystem::remove(directory, ec);
		}

		std::printf("MaterialTests: all checks passed\n");
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "MaterialTests failed: %s\n", exception.what());
		return 1;
	}
}
