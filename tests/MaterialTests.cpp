// D3:材质资产(解析/版本/默认值/夹紧/往返)与材质库(缓存/保存/热重载)回归。
#include "World/Renderer/Material.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/MaterialTextureCache.h"
#include "World/Renderer/TextureData.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

		// 8. W5-L1:AssetFileWatch 语义与 AssetFingerprint(基线/未决 debounce/同内容重写/
		// 改回已确认值撤销/集合同步升序)。测试文件落在 Game/assets 下的临时子目录,
		// 与引擎用同一条"相对内容根"的解析路径。
		{
			CHECK(std::filesystem::exists(std::filesystem::current_path() / "CMakeLists.txt"));
			std::error_code ec;
			const std::filesystem::path directory = std::filesystem::current_path() / "Game" / "assets"
				/ "material_hotreload_tmp";
			std::filesystem::create_directories(directory, ec);
			const std::string relative = "material_hotreload_tmp/watch_probe.txt";
			const std::filesystem::path full = std::filesystem::current_path() / "Game" / "assets" / relative;
			const auto writeText = [&full](const std::string& text)
			{
				std::ofstream file(full, std::ios::binary | std::ios::trunc);
				file << text;
				file.flush();
			};

			writeText("version A");
			const AssetFingerprint baseline = FingerprintAsset(relative);
			CHECK(baseline.Exists);
			CHECK(baseline.FromContent);

			// 首登记只建立基线,不报告;重复 Watch 用当前内容重置基线。
			AssetFileWatch watch(AssetFileWatch::kDefaultDebounceSeconds);
			watch.Watch(relative);
			watch.Watch(relative);
			CHECK(watch.Size() == 1);
			CHECK(watch.IsWatched(relative));
			CHECK(watch.Poll(1.0).empty());

			// 同内容重写(只动 mtime)不算变化:指纹仍是内容哈希且完全一致。
			std::this_thread::sleep_for(std::chrono::milliseconds(30));
			writeText("version A");
			const AssetFingerprint rewritten = FingerprintAsset(relative);
			CHECK(rewritten.Exists && rewritten.FromContent);
			CHECK(rewritten.Value == baseline.Value);
			CHECK(watch.Poll(1.0).empty());

			// 内容变化:必须连续稳定 debounce 秒后才报告一次(期间内容再变则重启计时)。
			writeText("version B");
			CHECK(watch.Poll(0.05).empty());   // 观察到新内容:建立未决,elapsed=0
			CHECK(watch.Poll(0.05).empty());   // 0.05s < 0.15s:继续等待
			CHECK(watch.Poll(0.05).empty());   // 0.10s < 0.15s:继续等待
			const std::vector<std::string> reported = watch.Poll(0.05);   // 0.15s:报告一次
			CHECK(reported.size() == 1);
			CHECK(reported[0] == relative);
			CHECK(watch.Poll(1.0).empty());    // 报告后该内容成为新基线

			// Watch() 重置基线 + 内容改回已确认值 → 撤销未决变化,不报告。
			writeText("version C");
			watch.Watch(relative);
			CHECK(watch.Poll(1.0).empty());
			writeText("version D");
			CHECK(watch.Poll(0.05).empty());   // 未决(尚未到 debounce)
			writeText("version C");            // 改回已确认内容
			CHECK(watch.Poll(1.0).empty());    // 撤销未决,不报告

			// WatchedPaths() 升序 + 集合同步。
			writeText("version E");
			watch.Watch("material_hotreload_tmp/watch_probe_extra.txt");
			const std::vector<std::string> watched = watch.WatchedPaths();
			CHECK(watched.size() == 2);
			CHECK(std::is_sorted(watched.begin(), watched.end()));
			CHECK(watched[0] == "material_hotreload_tmp/watch_probe.txt");

			// 空路径不产生条目。
			watch.Watch(std::string());
			CHECK(watch.Size() == 2);

			// 内容变化(内容 A 的哈希与基准不同):首次 Poll 建立未决,第二次过 debounce 报告。
			writeText("version F");
			CHECK(watch.Poll(0.05).empty());
			const std::vector<std::string> changedAgain = watch.Poll(0.5);
			CHECK(changedAgain.size() == 1);
			CHECK(changedAgain[0] == "material_hotreload_tmp/watch_probe.txt");

			watch.Clear();
			CHECK(watch.Size() == 0);
			CHECK(!watch.IsWatched(relative));

			std::filesystem::remove_all(directory, ec);
		}

		// 9. W5-L1:clean 材质的外部内容变化 → 自动原地重载(Ref 同一性保持、Revision 前进、
		// GetDesc() 为新内容;mtime 不变也靠内容哈希检出)。
		{
			std::error_code ec;
			const std::filesystem::path directory = std::filesystem::current_path() / "Game" / "assets"
				/ "material_hotreload_tmp";
			std::filesystem::create_directories(directory, ec);
			const std::string relative = "material_hotreload_tmp/hot_clean.wmat";
			const std::filesystem::path full = std::filesystem::current_path() / "Game" / "assets" / relative;
			MaterialDesc desc;
			desc.Name = "HotClean";
			desc.Roughness = 0.1f;
			{
				std::ofstream file(full, std::ios::binary | std::ios::trunc);
				file << MaterialIO::Serialize(desc);
			}

			MaterialLibrary& library = MaterialLibrary::Get();
			Ref<Material> material = library.Load(relative, nullptr);
			CHECK(material != nullptr);
			CHECK(material->GetDesc().Name == "HotClean");
			const Ref<Material> sameInstance = material;
			const uint32_t revisionBefore = material->GetRevision();

			// 先建立基线(首登记不报告),再模拟外部改写。
			library.PollAssetChanges(0.0, AssetHotReloadReport {});

			// 外部改写内容:显式把 mtime 设回原值,证明检测口径是内容哈希而不是时间戳。
			desc.Name = "HotCleanV2";
			desc.Roughness = 0.75f;
			const auto originalTime = std::filesystem::last_write_time(full);
			{
				std::ofstream file(full, std::ios::binary | std::ios::trunc);
				file << MaterialIO::Serialize(desc);
			}
			std::filesystem::last_write_time(full, originalTime, ec);
			CHECK(!ec);

			AssetHotReloadReport report;
			library.PollAssetChanges(0.0, report);
			CHECK(report.ReloadedMaterials.empty());   // 首次看到变化:只是未决
			CHECK(material->GetDesc().Name == "HotClean");
			report = AssetHotReloadReport {};
			library.PollAssetChanges(0.05, report);
			CHECK(report.ReloadedMaterials.empty());   // 0.05s 仍 < debounce
			report = AssetHotReloadReport {};
			library.PollAssetChanges(0.2, report);     // 过 debounce → 一次重载
			CHECK(report.ReloadedMaterials.size() == 1);
			CHECK(report.ReloadedMaterials[0] == relative);
			CHECK(report.SkippedDirtyMaterials.empty());
			CHECK(report.FailedMaterials.empty());
			CHECK(material == sameInstance);           // Ref 同一性保持
			CHECK(material->GetDesc().Name == "HotCleanV2");
			CHECK(material->GetDesc().Roughness > 0.74f);
			CHECK(material->GetRevision() > revisionBefore);

			// 同一内容轮询不再报告(该内容已成为新基线)。
			report = AssetHotReloadReport {};
			library.PollAssetChanges(1.0, report);
			CHECK(!report.Any());

			std::filesystem::remove_all(directory, ec);
			library.Shutdown();   // 清缓存,避免删除的临时资产污染后续用例
		}

		// 10. W5-L1:dirty 材质的外部变化只报告不覆盖;坏文件失败保留旧 desc。
		{
			std::error_code ec;
			const std::filesystem::path directory = std::filesystem::current_path() / "Game" / "assets"
				/ "material_hotreload_tmp";
			std::filesystem::create_directories(directory, ec);
			const std::string dirtyPath = "material_hotreload_tmp/hot_dirty.wmat";
			const std::string brokenPath = "material_hotreload_tmp/hot_broken.wmat";
			const std::filesystem::path dirtyFull = std::filesystem::current_path() / "Game" / "assets" / dirtyPath;
			const std::filesystem::path brokenFull = std::filesystem::current_path() / "Game" / "assets" / brokenPath;

			MaterialDesc dirtyDesc;
			dirtyDesc.Name = "DirtyBase";
			{
				std::ofstream file(dirtyFull, std::ios::binary | std::ios::trunc);
				file << MaterialIO::Serialize(dirtyDesc);
			}
			MaterialDesc brokenDesc;
			brokenDesc.Name = "BrokenBase";
			{
				std::ofstream file(brokenFull, std::ios::binary | std::ios::trunc);
				file << MaterialIO::Serialize(brokenDesc);
			}

			MaterialLibrary& library = MaterialLibrary::Get();
			Ref<Material> dirty = library.Load(dirtyPath, nullptr);
			Ref<Material> broken = library.Load(brokenPath, nullptr);
			CHECK(dirty != nullptr && broken != nullptr);

			// 先跑一次轮询让两个新路径进入监听集合(首登记只建立基线,不报告)——
			// 之后的磁盘改写才会被检出。
			library.PollAssetChanges(0.0, AssetHotReloadReport {});

			// 外部改写两份文件;dirty 材质带未保存修改(编辑器面板语义)。
			dirtyDesc.Name = "DiskDirty";
			brokenDesc.Name = "DiskBroken";
			{
				std::ofstream file(dirtyFull, std::ios::binary | std::ios::trunc);
				file << MaterialIO::Serialize(dirtyDesc);
			}
			{
				std::ofstream file(brokenFull, std::ios::binary | std::ios::trunc);
				file << MaterialIO::Serialize(brokenDesc);
			}
			dirty->SetRoughness(0.33f);
			dirty->MarkDirty(true);
			const std::string dirtyInMemoryName = dirty->GetDesc().Name;

			library.PollAssetChanges(0.05, AssetHotReloadReport {});
			AssetHotReloadReport report;
			library.PollAssetChanges(0.5, report);
			CHECK(report.SkippedDirtyMaterials.size() == 1);
			CHECK(report.SkippedDirtyMaterials[0] == dirtyPath);
			CHECK(std::find(report.ReloadedMaterials.begin(), report.ReloadedMaterials.end(), dirtyPath)
				== report.ReloadedMaterials.end());
			CHECK(report.ReloadedMaterials.size() == 1);   // broken 材质是 clean → 自动重载
			CHECK(report.ReloadedMaterials[0] == brokenPath);
			CHECK(report.FailedMaterials.empty());
			CHECK(dirty->GetDesc().Name == dirtyInMemoryName);   // 内存态未被覆盖
			CHECK(dirty->IsDirty());                              // 脏标记保持

			// 坏文件:替换为无法解析的内容 → FailedMaterials + 保留旧 desc。
			{
				std::ofstream file(brokenFull, std::ios::binary | std::ios::trunc);
				file << "this is not a valid material\n";
			}
			const std::string brokenName = broken->GetDesc().Name;
			library.PollAssetChanges(0.05, AssetHotReloadReport {});
			report = AssetHotReloadReport {};
			library.PollAssetChanges(0.5, report);
			CHECK(report.FailedMaterials.size() == 1);
			CHECK(report.FailedMaterials[0].Path == brokenPath);
			CHECK(!report.FailedMaterials[0].Error.empty());
			CHECK(broken->GetDesc().Name == brokenName);   // 旧 desc 保留

			std::filesystem::remove_all(directory, ec);
			library.Shutdown();   // 清缓存 + 监听集合在下一轮清空
		}

		// 11. W5-L1 路径 bug 回归:普通 xxx.wmat(相对内容根)在文件更新后 IsFileNewer == true。
		// 修复前 FileWriteTime 拼的是 Game/ 而不是 Game/assets/,时间戳恒为 min()。
		{
			std::error_code ec;
			const std::filesystem::path directory = std::filesystem::current_path() / "Game" / "assets"
				/ "material_hotreload_tmp";
			std::filesystem::create_directories(directory, ec);
			const std::string relative = "material_hotreload_tmp/mtime_probe.wmat";
			const std::filesystem::path full = std::filesystem::current_path() / "Game" / "assets" / relative;
			MaterialDesc desc;
			desc.Name = "MtimeProbe";
			{
				std::ofstream file(full, std::ios::binary | std::ios::trunc);
				file << MaterialIO::Serialize(desc);
			}

			MaterialLibrary& library = MaterialLibrary::Get();
			Ref<Material> material = library.Load(relative, nullptr);
			CHECK(material != nullptr);
			CHECK(!library.IsFileNewer(*material));   // 刚加载:磁盘不比内存新

			std::this_thread::sleep_for(std::chrono::milliseconds(30));
			desc.Roughness = 0.42f;
			{
				std::ofstream file(full, std::ios::binary | std::ios::trunc);
				file << MaterialIO::Serialize(desc);
			}
			CHECK(library.IsFileNewer(*material));    // 修复后:磁盘时间戳真的比 m_FileTime 新

			std::filesystem::remove_all(directory, ec);
		}

		// 12. W5-L1 路径 bug 回归:无 Application 实例时 TextureData 的磁盘回退能吃内容根。
		// 修复前拼的是 Game/textures/Icon.png(不存在)→ 1x1 白纹理兜底(Valid=false)。
		{
			const TextureData data = LoadTextureData("textures/Icon.png", /*flipVertically*/ false);
			CHECK(data.Valid);
			CHECK(data.Width == 640);
			CHECK(data.Height == 640);
			CHECK(data.Pixels.size() == static_cast<size_t>(640) * 640 * 4);

			const TextureData missing = LoadTextureData("textures/__no_such_texture__.png", false);
			CHECK(!missing.Valid);
			CHECK(missing.Width == 1 && missing.Height == 1);
			CHECK(missing.Pixels.size() == 4);
		}

		// 13. W5-L1:MaterialTextureCache::Invalidate 在无设备环境下安全 no-op(不崩、不误清)。
		{
			MaterialTextureCache& cache = MaterialTextureCache::Get();
			cache.Invalidate(std::string());                  // 空路径拒绝
			cache.Invalidate("textures/Icon.png");            // 未命中:安全
			cache.Invalidate("material_hotreload_tmp/none.png");
			cache.Clear();                                    // 无设备的 Clear 同样安全
			CHECK(true);                                      // 运行到这里 = 没有崩溃/未定义行为
		}

		// 14. U23:带长尾差的浮点("拖一下滑杆"的值)保存往返必须成功。
		// 修复前:FormatFloat 是 fixed + precision(6),0.1f + 0.2f(= 0.30000001192092896)
		// 写成 "0.300000",而 MaterialLibrary::Save 的回读校验按逐位相等比较 →
		// Save 返回 false(报"写入校验失败"),磁盘其实已写、脏标记永远清不掉。
		{
			CHECK(std::filesystem::exists(std::filesystem::current_path() / "CMakeLists.txt"));
			std::error_code ec;
			const std::filesystem::path directory = std::filesystem::current_path() / "Game" / "assets"
				/ "material_save_precision_tmp";
			std::filesystem::create_directories(directory, ec);
			const std::string relative = "material_save_precision_tmp/long_tail.wmat";
			const std::filesystem::path full = std::filesystem::current_path() / "Game" / "assets" / relative;

			MaterialLibrary& library = MaterialLibrary::Get();
			Ref<Material> material = library.CreateDefault("LongTail");
			CHECK(material != nullptr);
			MaterialDesc desc = material->GetDesc();
			// 0.4997164 = U22 实测"拖一下滑杆"的值;先证明旧的 6 位定点写出确实不可逆,
			// 再验证新写法读回来逐位相等(否则这条用例证明不了任何事)。
			desc.Roughness = 0.4997164f;
			desc.Metallic = 0.1234567f;
			desc.BaseColor = glm::vec4(1.0f / 3.0f, 0.4997164f, 0.7f, 1.0f);
			desc.Emissive = glm::vec3(0.0617284f, 0.05f, 0.0f);
			CHECK(std::strtof("0.499716", nullptr) != desc.Roughness);
			CHECK(std::strtof("0.123457", nullptr) != desc.Metallic);
			CHECK(std::strtof("0.333333", nullptr) != desc.BaseColor.x);
			material->SetDesc(desc);
			material->MarkDirty(true);

			std::string error;
			CHECK(library.Save(material, relative, &error));     // 修复前:false + "写入校验失败"
			CHECK(error.empty());                                 // 长尾值不是警告,更不是错误
			CHECK(!material->IsDirty());                          // 保存成功 = 脏标记清掉
			CHECK(material->GetPath() == relative);

			// 直接读磁盘断言"写出的文本读回来逐位相等"(不是只靠内存态)。
			std::string diskText;
			{
				std::ifstream file(full, std::ios::binary);
				diskText.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			}
			CHECK(!diskText.empty());
			MaterialDesc reloaded;
			CHECK(MaterialIO::Parse(diskText, reloaded, nullptr).Success);
			CHECK(reloaded.Roughness == desc.Roughness);
			CHECK(reloaded.Metallic == desc.Metallic);
			CHECK(reloaded.BaseColor == desc.BaseColor);
			CHECK(reloaded.Emissive == desc.Emissive);
			// 序列化文本必须带够位数(旧的 6 位定点只会写 "0.499716")。
			CHECK(diskText.find("0.4997164") != std::string::npos);
			CHECK(diskText.find("Roughness: 0.499716\n") == std::string::npos);

			std::filesystem::remove_all(directory, ec);
			library.Shutdown();
		}

		// 15. U23:回读校验的比较口径 —— 浮点按 1e-6 容差,非浮点字段严格相等。
		{
			MaterialDesc base;
			base.Name = "Tolerance";
			base.Roughness = 0.4997164f;    // 6 位小数写出会丢尾差
			base.Metallic = 0.5f;
			base.BaseColor = glm::vec4(1.0f / 3.0f, 0.25f, 0.5f, 1.0f);
			base.Emissive = glm::vec3(0.1f, 0.2f, 0.3f);
			base.AlbedoTexture = "textures/Icon.png";
			base.BlendMode = MaterialBlendMode::Transparent;
			base.DoubleSided = true;

			// 旧格式写出的 6 位小数文本回读后差 ~4e-7:容差内 → 等价(保存不再被判失败)。
			MaterialDesc truncated;
			CHECK(MaterialIO::Parse(
				"FormatVersion: 1\nName: \"Tolerance\"\nBaseColor: [0.333333, 0.25, 0.5, 1.0]\n"
				"Metallic: 0.5\nRoughness: 0.499716\nEmissive: [0.1, 0.2, 0.3]\n"
				"AlbedoTexture: \"textures/Icon.png\"\nNormalTexture: \"\"\n"
				"BlendMode: Transparent\nDoubleSided: true\n",
				truncated, nullptr).Success);
			CHECK(truncated != base);                                  // 逐位比较确实不相等
			CHECK(MaterialIO::EquivalentForSave(truncated, base));     // 容差比较通过

			// 超差数值必须被拒(容差不是"什么都放过")。
			MaterialDesc beyond = base;
			beyond.Roughness = base.Roughness + 1e-3f;
			CHECK(!MaterialIO::EquivalentForSave(beyond, base));
			MaterialDesc beyondColor = base;
			beyondColor.BaseColor.x = base.BaseColor.x + 1e-3f;
			CHECK(!MaterialIO::EquivalentForSave(beyondColor, base));
			// NaN 不被容差吞掉(旧逐位比较同样拒绝)。
			MaterialDesc nanValue = base;
			nanValue.Roughness = std::numeric_limits<float>::quiet_NaN();
			CHECK(!MaterialIO::EquivalentForSave(nanValue, base));

			// 非浮点字段严格相等:字符串 / 枚举 / 布尔 任一不同都不等价。
			MaterialDesc textureDiff = base;
			textureDiff.AlbedoTexture = "textures/quadrants.png";
			CHECK(!MaterialIO::EquivalentForSave(textureDiff, base));
			MaterialDesc normalDiff = base;
			normalDiff.NormalTexture = "textures/n.png";
			CHECK(!MaterialIO::EquivalentForSave(normalDiff, base));
			MaterialDesc blendDiff = base;
			blendDiff.BlendMode = MaterialBlendMode::Opaque;
			CHECK(!MaterialIO::EquivalentForSave(blendDiff, base));
			MaterialDesc sideDiff = base;
			sideDiff.DoubleSided = false;
			CHECK(!MaterialIO::EquivalentForSave(sideDiff, base));
			MaterialDesc nameDiff = base;
			nameDiff.Name = "Other";
			CHECK(!MaterialIO::EquivalentForSave(nameDiff, base));
		}

		// 16. U23:故意写坏的文件仍然必须失败(容差比较没有放宽"文件必须能解析"这一关)。
		{
			CHECK(std::filesystem::exists(std::filesystem::current_path() / "CMakeLists.txt"));
			std::error_code ec;
			const std::filesystem::path directory = std::filesystem::current_path() / "Game" / "assets"
				/ "material_save_precision_tmp";
			std::filesystem::create_directories(directory, ec);
			const std::string relative = "material_save_precision_tmp/broken.wmat";
			const std::filesystem::path full = std::filesystem::current_path() / "Game" / "assets" / relative;

			MaterialDesc good;
			good.Name = "GoodBase";
			{
				std::ofstream file(full, std::ios::binary | std::ios::trunc);
				file << MaterialIO::Serialize(good);
			}
			MaterialLibrary& library = MaterialLibrary::Get();
			Ref<Material> material = library.Load(relative, nullptr);
			CHECK(material != nullptr);
			CHECK(material->GetDesc().Name == "GoodBase");

			// ① 直接解析坏文本:必须失败(不是"回退默认值算成功")。
			MaterialDesc parsed;
			CHECK(!MaterialIO::Parse("this is not a valid material\n", parsed, nullptr).Success);
			// ② 字段类型写坏也要失败。
			CHECK(!MaterialIO::Parse("FormatVersion: 1\nMetallic: \"high\"\n", parsed, nullptr).Success);
			// ③ 不支持的版本要失败(不猜、不降级)。
			CHECK(!MaterialIO::Parse("FormatVersion: 99\nName: \"x\"\n", parsed, nullptr).Success);

			// ④ 库侧:坏文件 Reload 失败且不覆盖内存态(旧 desc 保留、仍可继续编辑)。
			{
				std::ofstream file(full, std::ios::binary | std::ios::trunc);
				file << "FormatVersion: 1\nMetallic: \"high\"\n";
			}
			std::string error;
			CHECK(!library.Reload(relative, &error));
			CHECK(!error.empty());
			CHECK(material->GetDesc().Name == "GoodBase");

			// ⑤ 坏文件永远解析不出材质(新路径 Load 也必须失败)。
			const std::string brokenNew = "material_save_precision_tmp/broken_new.wmat";
			{
				std::ofstream file(directory / "broken_new.wmat", std::ios::binary | std::ios::trunc);
				file << "not a material at all\n";
			}
			CHECK(library.Load(brokenNew, nullptr) == nullptr);

			std::filesystem::remove_all(directory, ec);
			library.Shutdown();
		}

		// ---- M3:材质实例(.wmat = 覆盖字段 + Parent)----
		//
		// 夹具统一落在 Game/assets/material_m3_tmp/(与引擎同一条"相对内容根"的解析路径),
		// 每个用例自己清理;父级链一律用**相对内容根**的路径书写。
		{
			CHECK(std::filesystem::exists(std::filesystem::current_path() / "CMakeLists.txt"));
			std::error_code ec;
			const std::filesystem::path directory = std::filesystem::current_path() / "Game" / "assets"
				/ "material_m3_tmp";
			std::filesystem::remove_all(directory, ec);
			std::filesystem::create_directories(directory, ec);
			const auto fullPath = [&directory](const std::string& fileName)
			{
				return directory / fileName;
			};
			const auto writeText = [&fullPath](const std::string& fileName, const std::string& text)
			{
				std::ofstream file(fullPath(fileName), std::ios::binary | std::ios::trunc);
				file << text;
				file.flush();
			};
			const auto readText = [&fullPath](const std::string& fileName)
			{
				std::ifstream file(fullPath(fileName), std::ios::binary);
				return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			};
			const auto relative = [](const std::string& fileName)
			{
				return "material_m3_tmp/" + fileName;
			};
			MaterialLibrary& library = MaterialLibrary::Get();
			std::string error;

			// 17. 解析优先级:本文件覆盖 → 父级(递归)→ 引擎内置默认(缺省 Parent 即默认)。
			{
				writeText("m3_grandparent.wmat",
					"FormatVersion: 2\nName: \"Grand\"\nBaseColor: [0.25, 0.5, 0.75, 1]\nRoughness: 0.75\n");
				writeText("m3_parent.wmat",
					"FormatVersion: 2\nParent: material_m3_tmp/m3_grandparent.wmat\n"
					"Name: \"Parent\"\nMetallic: 0.2\n");
				writeText("m3_child.wmat",
					"FormatVersion: 2\nParent: material_m3_tmp/m3_parent.wmat\n"
					"Name: \"Child\"\nMetallic: 0.9\nAlbedoTexture: \"textures/Icon.png\"\n");

				Ref<Material> child = library.Load(relative("m3_child.wmat"), &error);
				CHECK(child != nullptr);
				CHECK(error.empty());
				CHECK(child->GetDesc().Name == "Child");                       // 自己
				CHECK(child->GetDesc().Metallic == 0.9f);                      // 自己(盖住父级 0.2)
				CHECK(child->GetDesc().AlbedoTexture == "textures/Icon.png");  // 自己
				CHECK(child->GetDesc().Roughness == 0.75f);                    // 自己没写 → 祖父
				CHECK(child->GetDesc().BaseColor == glm::vec4(0.25f, 0.5f, 0.75f, 1.0f));   // 祖父
				CHECK(child->GetDesc().NormalTexture.empty());                 // 全链没写 → 引擎默认
				CHECK(child->GetDesc().BlendMode == MaterialBlendMode::Opaque);
				CHECK(!child->GetDesc().DoubleSided);
				CHECK(child->ParentPath() == relative("m3_parent.wmat"));
				CHECK(child->ResolvedParent() != nullptr);
				CHECK(child->ResolvedParent()->GetPath() == relative("m3_parent.wmat"));
				CHECK(library.Load(relative("m3_parent.wmat"), nullptr) == child->ResolvedParent());
				CHECK(child->ResolvedParent()->ParentPath() == relative("m3_grandparent.wmat"));
				CHECK(child->ResolvedParent()->ResolvedParent() != nullptr);
				CHECK(child->ResolvedParent()->GetDesc().Metallic == 0.2f);
				CHECK(child->ResolvedParent()->GetDesc().BaseColor == glm::vec4(0.25f, 0.5f, 0.75f, 1.0f));
				CHECK(!child->IsParentMissing());
				CHECK(child->ParentWarning().empty());
				// 覆盖集 = 本文件写出来的字段(不是"合并后的全部字段")。
				CHECK(child->HasOverride(MaterialField::Name));
				CHECK(child->HasOverride(MaterialField::Metallic));
				CHECK(child->HasOverride(MaterialField::AlbedoTexture));
				CHECK(!child->HasOverride(MaterialField::Roughness));
				CHECK(!child->HasOverride(MaterialField::BaseColor));
				CHECK(child->OverrideCount() == 3);
				CHECK(child->GetFormatVersion() == 2);

				// 缺省 Parent = 引擎内置默认(用户确认的语义)。
				writeText("m3_rootless.wmat", "FormatVersion: 2\nMetallic: 0.4\n");
				Ref<Material> rootless = library.Load(relative("m3_rootless.wmat"), nullptr);
				CHECK(rootless != nullptr);
				CHECK(rootless->ParentPath().empty());
				CHECK(rootless->ResolvedParent() == nullptr);
				CHECK(!rootless->IsParentMissing());
				CHECK(rootless->GetDesc().Metallic == 0.4f);
				CHECK(rootless->GetDesc().Roughness == MaterialIO::DefaultMaterialDesc().Roughness);
				CHECK(rootless->GetDesc().BaseColor == MaterialIO::DefaultMaterialDesc().BaseColor);
				CHECK(rootless->GetDesc().Name == MaterialIO::DefaultMaterialDesc().Name);
				CHECK(rootless->OverrideCount() == 1);
				library.Shutdown();
			}

			// 18. 写盘只写覆盖字段 + Parent(逐字节);Save As 变体 = Parent 指向当前材质。
			{
				writeText("m3_writeparent.wmat", "FormatVersion: 2\nName: \"WriteParent\"\nRoughness: 0.6\n");

				Ref<Material> instance = library.CreateInstance(relative("m3_writeparent.wmat"),
					"WriteChild", &error);
				CHECK(instance != nullptr);
				CHECK(error.empty());
				CHECK(instance->IsDirty());
				CHECK(instance->GetDesc().Roughness == 0.6f);      // 初始 = 父级值
				CHECK(instance->GetDesc().Name == "WriteChild");
				instance->SetMetallic(0.9f);
				CHECK(instance->HasOverride(MaterialField::Metallic));
				CHECK(!instance->HasOverride(MaterialField::Roughness));
				CHECK(library.Save(instance, relative("m3_writechild.wmat"), &error));
				CHECK(error.empty());
				CHECK(!instance->IsDirty());

				const std::string text = readText("m3_writechild.wmat");
				CHECK(text == "FormatVersion: 2\n"
					"Parent: material_m3_tmp/m3_writeparent.wmat\n"
					"Name: \"WriteChild\"\n"
					"Metallic: 0.9\n");
				CHECK(text.find("Roughness") == std::string::npos);
				CHECK(text.find("BaseColor") == std::string::npos);
				CHECK(text.find("BlendMode") == std::string::npos);

				// 重开(缓存清空后从磁盘读):未覆盖字段 == 父级值,覆盖字段 == 自己的值。
				library.Shutdown();
				Ref<Material> reopened = library.Load(relative("m3_writechild.wmat"), &error);
				CHECK(reopened != nullptr);
				CHECK(reopened->GetDesc().Roughness == 0.6f);
				CHECK(reopened->GetDesc().Metallic == 0.9f);
				CHECK(reopened->GetDesc().Name == "WriteChild");

				// Save As 变体:Parent = 当前材质,产物只写自己的覆盖(不是整份拷贝)。
				Ref<Material> variant = library.CreateInstance(relative("m3_writechild.wmat"),
					"WriteVariant", &error);
				CHECK(variant != nullptr);
				CHECK(variant->GetDesc().Metallic == 0.9f);        // 从父级(子材质)解析出来的值
				CHECK(variant->GetDesc().Roughness == 0.6f);
				CHECK(library.Save(variant, relative("m3_writevariant.wmat"), &error));
				CHECK(readText("m3_writevariant.wmat") == "FormatVersion: 2\n"
					"Parent: material_m3_tmp/m3_writechild.wmat\n"
					"Name: \"WriteVariant\"\n");
				library.Shutdown();
			}

			// 19. 老 .wmat(v1 全字段)逐字节不变:Load → Save 的磁盘字节 == M3 前的写出。
			{
				MaterialDesc legacy;
				legacy.Name = "Legacy";
				legacy.BaseColor = glm::vec4(0.2f, 0.4f, 0.6f, 1.0f);
				legacy.Metallic = 0.35f;
				legacy.Roughness = 0.42f;
				legacy.Emissive = glm::vec3(0.1f, 0.0f, 0.05f);
				legacy.AlbedoTexture = "textures/Icon.png";
				legacy.NormalTexture = "textures/quadrants.png";
				legacy.BlendMode = MaterialBlendMode::Transparent;
				legacy.DoubleSided = true;
				const std::string legacyText = MaterialIO::Serialize(legacy);
				CHECK(legacyText.rfind("# WorldEngine 材质资产", 0) == 0);
				CHECK(legacyText.find("FormatVersion: 1\n") != std::string::npos);
				CHECK(legacyText.find("Parent:") == std::string::npos);
				CHECK(legacyText.find("BaseColor: [0.2, 0.4, 0.6, 1]\n") != std::string::npos);

				// 老文件解析出来 = 全部字段都是覆盖(所以合并结果与 M3 前逐字段一致)。
				MaterialDocument document;
				CHECK(MaterialIO::ParseDocument(legacyText, document, nullptr).Success);
				CHECK(document.Overridden.All());
				CHECK(document.Overridden.Count() == static_cast<int>(kMaterialFieldCount));
				CHECK(document.ParentPath.empty());
				CHECK(MaterialIO::DocumentFormatVersion(document) == 1);
				CHECK(MaterialIO::SerializeDocument(document) == legacyText);   // 逐字节往返
				CHECK(MaterialIO::MergeDocument(document, nullptr) == legacy);

				writeText("m3_legacy.wmat", legacyText);
				Ref<Material> loaded = library.Load(relative("m3_legacy.wmat"), &error);
				CHECK(loaded != nullptr);
				CHECK(error.empty());
				CHECK(loaded->GetDesc() == legacy);
				CHECK(loaded->GetFormatVersion() == 1);
				CHECK(loaded->OverrideCount() == static_cast<int>(kMaterialFieldCount));
				CHECK(loaded->ParentPath().empty());
				CHECK(loaded->ResolvedParent() == nullptr);
				CHECK(library.Save(loaded, relative("m3_legacy.wmat"), &error));
				CHECK(error.empty());
				CHECK(readText("m3_legacy.wmat") == legacyText);   // 未改动 → 逐字节不变
				library.Shutdown();
			}

			// 20. 父级缺失/坏 → 退化成引擎默认 + 可读警告,子材质仍可用。
			{
				writeText("m3_orphan.wmat",
					"FormatVersion: 2\nParent: material_m3_tmp/__missing_parent__.wmat\n"
					"Name: \"Orphan\"\nMetallic: 0.4\n");
				Ref<Material> orphan = library.Load(relative("m3_orphan.wmat"), &error);
				CHECK(orphan != nullptr);
				CHECK(!error.empty());                       // 成功但带警告
				CHECK(orphan->IsParentMissing());
				CHECK(!orphan->ParentWarning().empty());
				CHECK(orphan->ResolvedParent() == nullptr);
				CHECK(orphan->GetDesc().Name == "Orphan");   // 自己的覆盖照常生效
				CHECK(orphan->GetDesc().Metallic == 0.4f);
				CHECK(orphan->GetDesc().Roughness == MaterialIO::DefaultMaterialDesc().Roughness);
				CHECK(orphan->GetDesc().BaseColor == MaterialIO::DefaultMaterialDesc().BaseColor);
				CHECK(library.GetLoadWarning(relative("m3_orphan.wmat")).find("父级") != std::string::npos);

				// 父级存在但解析不了:同口径(退化 + 警告,不失败)。
				writeText("m3_broken_parent.wmat", "this is not a material\n");
				writeText("m3_broken_child.wmat",
					"FormatVersion: 2\nParent: material_m3_tmp/m3_broken_parent.wmat\nMetallic: 0.3\n");
				Ref<Material> brokenChild = library.Load(relative("m3_broken_child.wmat"), &error);
				CHECK(brokenChild != nullptr);
				CHECK(!error.empty());
				CHECK(brokenChild->IsParentMissing());
				CHECK(brokenChild->GetDesc().Metallic == 0.3f);
				CHECK(brokenChild->GetDesc().Roughness == MaterialIO::DefaultMaterialDesc().Roughness);
				library.Shutdown();
			}

			// 21. 父级循环引用 → 拒绝 + 可读链路(含环上每个路径);深度上限 8 级。
			{
				writeText("m3_cycle_a.wmat", "FormatVersion: 2\nParent: material_m3_tmp/m3_cycle_b.wmat\n");
				writeText("m3_cycle_b.wmat", "FormatVersion: 2\nParent: material_m3_tmp/m3_cycle_a.wmat\n");
				CHECK(library.Load(relative("m3_cycle_a.wmat"), &error) == nullptr);
				CHECK(error.find("循环") != std::string::npos);
				CHECK(error.find("m3_cycle_a.wmat") != std::string::npos);
				CHECK(error.find("m3_cycle_b.wmat") != std::string::npos);
				// 自引用同样是循环(不会栈溢出 / 死循环)。
				writeText("m3_selfcycle.wmat", "FormatVersion: 2\nParent: material_m3_tmp/m3_selfcycle.wmat\n");
				CHECK(library.Load(relative("m3_selfcycle.wmat"), &error) == nullptr);
				CHECK(error.find("循环") != std::string::npos);
				CHECK(error.find("m3_selfcycle.wmat") != std::string::npos);

				// 深度:8 级父级允许,9 级拒绝(可读错误,不崩)。
				const auto writeChain = [&writeText](int level)
				{
					std::string text = "FormatVersion: 2\n";
					if (level > 0)
						text += "Parent: material_m3_tmp/m3_depth_" + std::to_string(level - 1) + ".wmat\n";
					text += "Metallic: 0.5\n";
					writeText("m3_depth_" + std::to_string(level) + ".wmat", text);
				};
				for (int level = 0; level <= 9; ++level)
					writeChain(level);
				library.Shutdown();
				Ref<Material> deepOk = library.Load(relative("m3_depth_8.wmat"), &error);
				CHECK(deepOk != nullptr);
				CHECK(deepOk->GetDesc().Metallic == 0.5f);
				library.Shutdown();
				CHECK(library.Load(relative("m3_depth_9.wmat"), &error) == nullptr);
				CHECK(error.find("超过") != std::string::npos);
				CHECK(error.find("m3_depth_9.wmat") != std::string::npos);
				library.Shutdown();
			}

			// 22. RevertField:回父级值 + 覆盖位消失(文件里那一行也消失,逐字节比对)。
			{
				writeText("m3_revert_parent.wmat",
					"FormatVersion: 2\nName: \"RevertParent\"\nMetallic: 0.2\nRoughness: 0.8\n");
				Ref<Material> child = library.CreateInstance(relative("m3_revert_parent.wmat"),
					"RevertChild", &error);
				CHECK(child != nullptr);
				child->SetMetallic(0.7f);
				CHECK(library.Save(child, relative("m3_revert_child.wmat"), &error));
				CHECK(readText("m3_revert_child.wmat").find("Metallic: 0.7\n") != std::string::npos);

				const uint32_t revisionBefore = child->GetRevision();
				CHECK(child->HasOverride(MaterialField::Metallic));
				CHECK(!child->HasOverride(MaterialField::Roughness));
				child->RevertField(MaterialField::Roughness);     // 本来就是继承态 → no-op
				CHECK(child->GetRevision() == revisionBefore);
				CHECK(!child->IsDirty());
				child->RevertField(MaterialField::Metallic);      // 覆盖 → 回父级值
				CHECK(!child->HasOverride(MaterialField::Metallic));
				CHECK(child->GetDesc().Metallic == 0.2f);          // 父级值
				CHECK(child->GetRevision() > revisionBefore);
				CHECK(child->IsDirty());
				CHECK(library.Save(child, relative("m3_revert_child.wmat"), &error));
				CHECK(!child->IsDirty());
				CHECK(readText("m3_revert_child.wmat") == "FormatVersion: 2\n"
					"Parent: material_m3_tmp/m3_revert_parent.wmat\n"
					"Name: \"RevertChild\"\n");
				// 重开确认:读回就是父级值(不是"把父级值写进文件")。
				library.Shutdown();
				Ref<Material> reopened = library.Load(relative("m3_revert_child.wmat"), &error);
				CHECK(reopened != nullptr);
				CHECK(reopened->GetDesc().Metallic == 0.2f);
				CHECK(!reopened->HasOverride(MaterialField::Metallic));
				library.Shutdown();
			}

			// 23. 指纹:子材质 = 本文件内容 ⊕ 解析后父级链(父改 → 子失效;子文件一字不动)。
			{
				const std::string parentOriginal =
					"FormatVersion: 2\nName: \"FpParent\"\nMetallic: 0.1\nRoughness: 0.5\n";
				writeText("m3_fp_parent.wmat", parentOriginal);
				writeText("m3_fp_child.wmat",
					"FormatVersion: 2\nParent: material_m3_tmp/m3_fp_parent.wmat\nName: \"FpChild\"\n");

				const AssetFingerprint childBefore = FingerprintAsset(relative("m3_fp_child.wmat"), &error);
				CHECK(childBefore.Exists);
				CHECK(childBefore.FromContent);
				CHECK(childBefore.Value != 0);
				CHECK(FingerprintAsset(relative("m3_fp_child.wmat"), nullptr).Value == childBefore.Value);
				const AssetFingerprint parentBefore = FingerprintAsset(relative("m3_fp_parent.wmat"), nullptr);

				writeText("m3_fp_parent.wmat",
					"FormatVersion: 2\nName: \"FpParent\"\nMetallic: 0.65\nRoughness: 0.5\n");
				const AssetFingerprint childAfter = FingerprintAsset(relative("m3_fp_child.wmat"), &error);
				CHECK(childAfter.Exists);
				CHECK(childAfter.Value != childBefore.Value);
				CHECK(FingerprintAsset(relative("m3_fp_parent.wmat"), nullptr).Value != parentBefore.Value);

				// 父级消失(链断裂)同样改变子指纹。
				std::filesystem::remove(fullPath("m3_fp_parent.wmat"), ec);
				const AssetFingerprint childOrphan = FingerprintAsset(relative("m3_fp_child.wmat"), nullptr);
				CHECK(childOrphan.Exists);
				CHECK(childOrphan.Value != childAfter.Value);
				// 父级还原 → 指纹回到第一次的值(纯函数、可复现)。
				writeText("m3_fp_parent.wmat", parentOriginal);
				CHECK(FingerprintAsset(relative("m3_fp_child.wmat"), nullptr).Value == childBefore.Value);
			}

			// 24. 父级改动经热重载链传播:子材质原地更新(同一性保持、Revision 前进);
			//     dirty 子材质只报告、绝不覆盖未保存修改。
			{
				writeText("m3_hot_parent.wmat", "FormatVersion: 2\nName: \"HotParent\"\nRoughness: 0.5\n");
				writeText("m3_hot_child.wmat",
					"FormatVersion: 2\nParent: material_m3_tmp/m3_hot_parent.wmat\nName: \"HotChild\"\n");
				library.Shutdown();
				library.PollAssetChanges(0.0, AssetHotReloadReport {});   // 清掉上一批的监听集合
				Ref<Material> child = library.Load(relative("m3_hot_child.wmat"), nullptr);
				Ref<Material> parent = library.Load(relative("m3_hot_parent.wmat"), nullptr);
				CHECK(child != nullptr && parent != nullptr);
				CHECK(child->GetDesc().Roughness == 0.5f);
				const Ref<Material> sameChild = child;
				const uint32_t childRevision = child->GetRevision();
				library.PollAssetChanges(0.0, AssetHotReloadReport {});   // 建立基线(首次登记不报告)

				writeText("m3_hot_parent.wmat", "FormatVersion: 2\nName: \"HotParent\"\nRoughness: 0.9\n");
				AssetHotReloadReport report;
				library.PollAssetChanges(0.05, report);
				CHECK(report.ReloadedMaterials.empty());                  // 0.05s < debounce
				report = AssetHotReloadReport {};
				library.PollAssetChanges(0.5, report);
				CHECK(report.ReloadedMaterials.size() >= 1);
				CHECK(report.FailedMaterials.empty());
				CHECK(child == sameChild);                                // 实例同一性保持
				CHECK(child->GetDesc().Roughness > 0.89f && child->GetDesc().Roughness < 0.91f);
				CHECK(child->GetRevision() > childRevision);
				CHECK(parent->GetDesc().Roughness > 0.89f && parent->GetDesc().Roughness < 0.91f);

				// dirty 子材质:父级再改 → 只报告,不覆盖未保存修改。
				child->SetMetallic(0.33f);
				child->MarkDirty(true);
				const std::string inMemoryName = child->GetDesc().Name;
				library.PollAssetChanges(0.0, AssetHotReloadReport {});
				writeText("m3_hot_parent.wmat", "FormatVersion: 2\nName: \"HotParent\"\nRoughness: 0.15\n");
				library.PollAssetChanges(0.05, AssetHotReloadReport {});
				report = AssetHotReloadReport {};
				library.PollAssetChanges(0.5, report);
				CHECK(std::find(report.SkippedDirtyMaterials.begin(), report.SkippedDirtyMaterials.end(),
					relative("m3_hot_child.wmat")) != report.SkippedDirtyMaterials.end());
				CHECK(child->GetDesc().Roughness > 0.89f && child->GetDesc().Roughness < 0.91f);  // 仍是旧值
				CHECK(child->GetDesc().Name == inMemoryName);
				CHECK(child->IsDirty());
				library.Shutdown();
			}

			std::filesystem::remove_all(directory, ec);
			library.Shutdown();
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
