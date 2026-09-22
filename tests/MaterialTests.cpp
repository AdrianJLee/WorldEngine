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

		std::printf("MaterialTests: all checks passed\n");
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "MaterialTests failed: %s\n", exception.what());
		return 1;
	}
}
