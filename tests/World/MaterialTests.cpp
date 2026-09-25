// D3:材质资产(解析/版本/默认值/夹紧/往返)与材质库(缓存/保存/热重载)回归。
#include "World/Renderer/Material.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/MaterialTextureCache.h"
#include "World/Renderer/TextureData.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
			// 沙箱目录放在内容根(WLD_ASSETPATH)下的临时子目录:MaterialIO 的读写路径约定是
			// "相对内容根",测试与引擎用同一条解析路径才有意义。
			// 前置:内容根由编译期宏 WLD_ASSETPATH 给出(绝对路径),与进程 CWD 无关。
			CHECK(std::filesystem::exists(std::filesystem::current_path() / "CMakeLists.txt"));
			const std::filesystem::path directory = std::filesystem::path(WLD_ASSETPATH)
				/ "material_tests_tmp";
			std::error_code ec;
			std::filesystem::create_directories(directory, ec);
			const std::string relative = "material_tests_tmp/library_case.wmat";
			const std::filesystem::path relativeFull = std::filesystem::path(WLD_ASSETPATH) / relative;
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
			// 直接读磁盘断言写出的内容(ReadFileText 的路径约定是相对内容根,
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
			CHECK(library.Load(copyPath, nullptr) == loaded);   // 路径已被规范化为内容根相对
			// Save As 后旧文件仍在磁盘上(不是删除语义),测试自行清理,免得留下垃圾资产。
			std::filesystem::remove(relativeFull, ec);

			std::filesystem::remove(directory / "library_copy.wmat", ec);
			std::filesystem::remove(directory / "library_copy.wmat.tmp", ec);
			std::filesystem::remove(directory, ec);
		}

		// 8. W5-L1:AssetFileWatch 语义与 AssetFingerprint(基线/未决 debounce/同内容重写/
		// 改回已确认值撤销/集合同步升序)。测试文件落在内容根下的临时子目录,
		// 与引擎用同一条"相对内容根"的解析路径。
		{
			CHECK(std::filesystem::exists(std::filesystem::current_path() / "CMakeLists.txt"));
			std::error_code ec;
			const std::filesystem::path directory = std::filesystem::path(WLD_ASSETPATH)
				/ "material_hotreload_tmp";
			std::filesystem::create_directories(directory, ec);
			const std::string relative = "material_hotreload_tmp/watch_probe.txt";
			const std::filesystem::path full = std::filesystem::path(WLD_ASSETPATH) / relative;
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
			const std::filesystem::path directory = std::filesystem::path(WLD_ASSETPATH)
				/ "material_hotreload_tmp";
			std::filesystem::create_directories(directory, ec);
			const std::string relative = "material_hotreload_tmp/hot_clean.wmat";
			const std::filesystem::path full = std::filesystem::path(WLD_ASSETPATH) / relative;
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
			const std::filesystem::path directory = std::filesystem::path(WLD_ASSETPATH)
				/ "material_hotreload_tmp";
			std::filesystem::create_directories(directory, ec);
			const std::string dirtyPath = "material_hotreload_tmp/hot_dirty.wmat";
			const std::string brokenPath = "material_hotreload_tmp/hot_broken.wmat";
			const std::filesystem::path dirtyFull = std::filesystem::path(WLD_ASSETPATH) / dirtyPath;
			const std::filesystem::path brokenFull = std::filesystem::path(WLD_ASSETPATH) / brokenPath;

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
			const std::filesystem::path directory = std::filesystem::path(WLD_ASSETPATH)
				/ "material_hotreload_tmp";
			std::filesystem::create_directories(directory, ec);
			const std::string relative = "material_hotreload_tmp/mtime_probe.wmat";
			const std::filesystem::path full = std::filesystem::path(WLD_ASSETPATH) / relative;
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
			const std::filesystem::path directory = std::filesystem::path(WLD_ASSETPATH)
				/ "material_save_precision_tmp";
			std::filesystem::create_directories(directory, ec);
			const std::string relative = "material_save_precision_tmp/long_tail.wmat";
			const std::filesystem::path full = std::filesystem::path(WLD_ASSETPATH) / relative;

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
			const std::filesystem::path directory = std::filesystem::path(WLD_ASSETPATH)
				/ "material_save_precision_tmp";
			std::filesystem::create_directories(directory, ec);
			const std::string relative = "material_save_precision_tmp/broken.wmat";
			const std::filesystem::path full = std::filesystem::path(WLD_ASSETPATH) / relative;

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
		// 夹具统一落在内容根下的 material_m3_tmp/(与引擎同一条"相对内容根"的解析路径),
		// 每个用例自己清理;父级链一律用**相对内容根**的路径书写。
		{
			CHECK(std::filesystem::exists(std::filesystem::current_path() / "CMakeLists.txt"));
			std::error_code ec;
			const std::filesystem::path directory = std::filesystem::path(WLD_ASSETPATH)
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

		// ---- M4-S2:注解参数表 + .wmat 的 Shader/Params ----
		//
		// 夹具落在内容根下的 material_m4s2_tmp/(与引擎同一条"相对内容根"解析路径):
			//  - 注解解析 / 值归一 / 反射(纯函数)不需要编译器,在本目标里跑;
			//  - 需要真实 slangc 的反射校验与编译在 World.ShaderPipeline 里(见 ShaderPipelineTests.cpp)。
		{
			using World::MaterialParamDecl;
			using World::MaterialParamSource;
			using World::ParamType;

			// 22. 注解解析:全类型 + 范围 / 单位 / 分组 / 标签 / 默认值归一。
			{
				const std::string source =
					"//! param Float Roughness = 0.4 [0,1] group(\"Surface\") label(\"Roughness\")\n"
					"//! param Vec2 Tiling = 1, 2 unit(\"tiles\") group(\"UV\")\n"
					"//! param Vec3 Tint = 1, 0.5, 0.25\n"
					"//! param Vec4 Weights = 1, 1, 1, 0.5f\n"
					"//! param Color Base = 0.8,0.2,0.1\n"
					"//! param Int Steps = 3 [0,8] unit(\"steps\")\n"
					"//! param Bool Glow = true group(\"Emissive\")\n"
					"//! param Texture2D Albedo = \"textures/icon.png\" label(\"Albedo\") doc(\"Albedo map (RGBA); 空 = 白色\")\n"
					"//! param Texture2D NoMap = \"\"\n"
					"Surface Evaluate(MaterialInputs input)\n"
					"{\n"
					"    return MakeDefaultSurface();\n"
					"}\n";
				std::vector<MaterialParamDecl> table;
				std::string error = "sentinel";
				if (!ParseMaterialParams(source, &table, &error))
					std::printf("MaterialTests: M4-S2 annotation parse failed: %s\n", error.c_str());
				CHECK(ParseMaterialParams(source, &table, &error));
				CHECK(error.empty());
				CHECK(table.size() == 9);
				CHECK(table[0].Name == "Roughness" && table[0].Type == ParamType::Float);
				CHECK(table[0].Default == "0.4");
				CHECK(table[0].Min == 0.0f && table[0].Max == 1.0f);
				CHECK(table[0].Group == "Surface" && table[0].Label == "Roughness");
				CHECK(table[0].Unit.empty());
				CHECK(table[0].Doc.empty());
				CHECK(table[1].Type == ParamType::Vec2 && table[1].Default == "1, 2");
				CHECK(table[1].Unit == "tiles" && table[1].Group == "UV");
				CHECK(table[2].Type == ParamType::Vec3 && table[2].Default == "1, 0.5, 0.25");
				CHECK(table[3].Type == ParamType::Vec4 && table[3].Default == "1, 1, 1, 0.5");
				CHECK(table[4].Type == ParamType::Color && table[4].Default == "0.8, 0.2, 0.1, 1");
				CHECK(table[5].Type == ParamType::Int && table[5].Default == "3");
				CHECK(table[5].Min == 0.0f && table[5].Max == 8.0f && table[5].Unit == "steps");
				CHECK(table[6].Type == ParamType::Bool && table[6].Default == "true");
				CHECK(table[7].Type == ParamType::Texture2D && table[7].Default == "textures/icon.png");
				CHECK(table[7].Doc == "Albedo map (RGBA); 空 = 白色");
				CHECK(table[8].Type == ParamType::Texture2D && table[8].Default.empty());

				// 注解写出 → 读回:编辑器改默认值/文案时走这条路径,口径必须闭环。
				for (const MaterialParamDecl& decl : table)
				{
					const std::string line = "//! " + FormatMaterialParamAnnotation(decl) + "\n";
					std::vector<MaterialParamDecl> reparsed;
					CHECK(ParseMaterialParams(line, &reparsed, &error));
					CHECK(reparsed.size() == 1);
					CHECK(reparsed[0].Name == decl.Name && reparsed[0].Type == decl.Type);
					CHECK(reparsed[0].Default == decl.Default);
					CHECK(reparsed[0].Min == decl.Min && reparsed[0].Max == decl.Max);
					CHECK(reparsed[0].Unit == decl.Unit && reparsed[0].Group == decl.Group
						&& reparsed[0].Label == decl.Label && reparsed[0].Doc == decl.Doc);
				}

				// 空源 / 没有注解 → 空表(不是错误)。
				std::vector<MaterialParamDecl> empty;
				CHECK(ParseMaterialParams("Surface Evaluate(MaterialInputs input) { return MakeDefaultSurface(); }\n",
					&empty, &error));
				CHECK(empty.empty());
			}

			// 23. 坏注解:每条都给 `<行>:<列>: <原因>`,列号指向出错的 token。
			{
				const auto expectError = [](const std::string& source, const std::string& reasonHint,
					const std::string& linePrefix)
				{
					std::vector<MaterialParamDecl> table;
					std::string error;
					const bool parsed = ParseMaterialParams(source, &table, &error);
					if (parsed || !table.empty() || error.rfind(linePrefix, 0) != 0
						|| error.find(reasonHint) == std::string::npos)
					{
						std::printf("MaterialTests: M4-S2 want '%s' @ '%s', got: %s\n",
							reasonHint.c_str(), linePrefix.c_str(), error.c_str());
					}
					CHECK(!parsed);
					CHECK(table.empty());
					CHECK(error.rfind(linePrefix, 0) == 0);
					CHECK(error.find(reasonHint) != std::string::npos);
					return error;
				};

				// 未知类型:列号 = 类型 token 的第一列。
				const std::string unknownType = expectError("//! param Float3 Tint = 1\n",
					"未知参数类型", "1:11:");
				CHECK(unknownType.find("Float3") != std::string::npos);
				expectError("//! param floats Tint = 1\n", "未知参数类型", "1:11:");
				// 未知指令 / 缺指令。
				expectError("//! colour Float Tint = 1\n", "未知注解指令", "1:5:");
				expectError("//! = 1\n", "缺少注解指令", "1:5:");
				// 缺默认值(= 号缺失 / 等号后为空)。
				expectError("//! param Float Tint\n", "缺少默认值", "1:");
				expectError("//! param Float Tint = [0,1]\n", "缺少默认值", "1:22:");
				// 重复参数名:第二条给第二次出现的行列。
				expectError("//! param Float Tint = 1\n//! param Vec3 Tint = 1, 1, 1\n",
					"重复的参数名", "2:16:");
				// 未知字段 / 字段重复 / 引号缺失。
				expectError("//! param Float Tint = 1 colour(\"x\")\n", "无法识别的注解内容", "1:");
				expectError("//! param Float Tint = 1 [0,1] [0,2]\n", "重复", "1:");
				expectError("//! param Float Tint = 1 unit(px)\n", "需要双引号字符串", "1:");
				// 范围:只给 Float/Int;下界不能大于上界;默认值必须在区间内。
				expectError("//! param Vec3 Tint = 1, 1, 1 [0,1]\n", "只适用于 Float/Int", "1:");
				expectError("//! param Float Tint = 1 [1,0]\n", "大于上界", "1:");
				expectError("//! param Float Tint = 2 [0,1]\n", "超出范围", "1:");
				// 名字:非法标识符 / 与引擎模板保留名冲突(u_ 前缀与模板标识符)。
				expectError("//! param Float 2Tint = 1\n", "缺少参数名", "1:17:");
				expectError("//! param Float u_Speed = 1\n", "保留名", "1:17:");
				expectError("//! param Float input = 1\n", "保留名", "1:17:");
				// 值文本与类型不符(默认值本身非法)。
				expectError("//! param Vec3 Tint = 1, 1\n", "不合法", "1:");
				expectError("//! param Bool Glow = yes\n", "不合法", "1:");
			}

			// 24. 值文本归一 + 浮点最短往返(与 .wmat 写出共用一套口径)。
			{
				std::string normalized;
				std::string error;
				CHECK(NormalizeParamValue(ParamType::Float, "0.500f", &normalized, &error)
					&& normalized == "0.5");
				CHECK(NormalizeParamValue(ParamType::Int, "3.0", &normalized, &error)
					&& normalized == "3");
				CHECK(NormalizeParamValue(ParamType::Bool, "TRUE", &normalized, &error)
					&& normalized == "true");
				CHECK(NormalizeParamValue(ParamType::Vec2, "1,2", &normalized, &error)
					&& normalized == "1, 2");
				CHECK(NormalizeParamValue(ParamType::Color, "1,0.5,0.25", &normalized, &error)
					&& normalized == "1, 0.5, 0.25, 1");
				CHECK(NormalizeParamValue(ParamType::Texture2D, "\"textures/x.png\"", &normalized, &error)
					&& normalized == "textures/x.png");
				CHECK(NormalizeParamValue(ParamType::Texture2D, "", &normalized, &error) && normalized.empty());
				CHECK(!NormalizeParamValue(ParamType::Float, "abc", &normalized, &error));
				CHECK(!NormalizeParamValue(ParamType::Vec3, "1, 2", &normalized, &error));
				CHECK(!NormalizeParamValue(ParamType::Int, "1.5", &normalized, &error));
				CHECK(!NormalizeParamValue(ParamType::Bool, "yes", &normalized, &error));
				CHECK(FormatParamFloatText(0.4997164f) == "0.4997164");
				CHECK(FormatParamFloatText(0.5f) == "0.5");
				// 反射类型 ↔ 注解类型的判据(HLSL bool 在 SPIR-V 里是 uint;
				// Color 与 Vec4 都是 v4float)。
				CHECK(IsReflectedTypeCompatible(ParamType::Float, "float"));
				CHECK(!IsReflectedTypeCompatible(ParamType::Float, "v2float"));
				CHECK(IsReflectedTypeCompatible(ParamType::Vec2, "v2float"));
				CHECK(IsReflectedTypeCompatible(ParamType::Vec3, "v3float"));
				CHECK(IsReflectedTypeCompatible(ParamType::Vec4, "v4float"));
				CHECK(IsReflectedTypeCompatible(ParamType::Color, "v4float"));
				CHECK(IsReflectedTypeCompatible(ParamType::Int, "int"));
				CHECK(!IsReflectedTypeCompatible(ParamType::Int, "uint"));
				CHECK(IsReflectedTypeCompatible(ParamType::Bool, "uint"));
				CHECK(!IsReflectedTypeCompatible(ParamType::Bool, "int"));
			}

			// 24b. MAT-UI7a:注解 `doc("…")`(参数说明)+ 行尾 `//` 注释。
			{
				// 文本协议:`doc(...)` 与其它字段同级、顺序任意、可省略;字符串规则与既有字段一致
				// (引号内的 `)` 不参与结构解析;`\"` / `\\` / `\n` 按既有转义读)。
				const std::string source =
					"//! param Float Roughness = 0.4 [0,1] unit(\"m\") group(\"Surface\") "
						"label(\"Roughness\") doc(\"表面粗糙度:0 = 镜面。\")\n"
					"//! param Vec2 Tiling = 1, 2 doc(\"UV 密度\") unit(\"tiles\")\n"          // 字段顺序任意
					"//! param Bool Glow = true // 行尾注释:说明不一定要写进 doc()\n"
					"//! param Texture2D Albedo = \"textures//icon.png\" doc(\"路径里的 // 原样保留\")\n"
					"//! param Int Steps = 3 [0,8] doc(\"clamp(x, 0, 1) 里的 ) 不需要转义\")\n"
					"//! param Float Escaped = 0.5 doc(\"带引号 \\\" 与反斜杠 \\\\ 的说明\")\n";
				std::vector<MaterialParamDecl> table;
				std::string error = "sentinel";
				if (!ParseMaterialParams(source, &table, &error))
					std::printf("MaterialTests: MAT-UI7a doc parse failed: %s\n", error.c_str());
				CHECK(ParseMaterialParams(source, &table, &error));
				CHECK(error.empty());
				CHECK(table.size() == 6);
				CHECK(table[0].Doc == "表面粗糙度:0 = 镜面。");
				CHECK(table[0].Unit == "m" && table[0].Group == "Surface"
					&& table[0].Label == "Roughness");
				CHECK(table[1].Type == ParamType::Vec2 && table[1].Default == "1, 2");
				CHECK(table[1].Doc == "UV 密度" && table[1].Unit == "tiles");
				CHECK(table[2].Default == "true" && table[2].Doc.empty());
				CHECK(table[3].Default == "textures//icon.png");        // 引号里的 // 不是注释
				CHECK(table[3].Doc == "路径里的 // 原样保留");
				CHECK(table[4].Doc == "clamp(x, 0, 1) 里的 ) 不需要转义");
				CHECK(table[5].Doc == "带引号 \" 与反斜杠 \\ 的说明");

				// 规范顺序的写出往返:文本、空格、引号逐字节一致(doc 写在最后,不动既有字段的排版)。
				// 范围故意用非默认值 [0.02,1] —— [0,1] 与"没写范围"等价,写出时会被省略。
				const std::string canonical =
					"//! param Float Roughness = 0.4 [0.02,1] unit(\"m\") group(\"Surface\") "
						"label(\"Roughness\") doc(\"表面粗糙度:0 = 镜面。\")\n";
				std::vector<MaterialParamDecl> canonicalTable;
				CHECK(ParseMaterialParams(canonical, &canonicalTable, &error));
				CHECK(canonicalTable.size() == 1);
				const std::string written =
					"//! " + FormatMaterialParamAnnotation(canonicalTable[0]) + "\n";
				CHECK(written == canonical);

				// 非规范顺序 / 含转义的文本:写出会规范化字段顺序并转义,但值逐字节保留;
				// 再写一次结果不变(幂等)。
				for (const MaterialParamDecl& decl : table)
				{
					const std::string first = FormatMaterialParamAnnotation(decl);
					std::vector<MaterialParamDecl> reparsed;
					CHECK(ParseMaterialParams("//! " + first + "\n", &reparsed, &error));
					CHECK(reparsed.size() == 1);
					CHECK(reparsed[0].Name == decl.Name && reparsed[0].Type == decl.Type);
					CHECK(reparsed[0].Default == decl.Default);
					CHECK(reparsed[0].Unit == decl.Unit && reparsed[0].Group == decl.Group
						&& reparsed[0].Label == decl.Label && reparsed[0].Doc == decl.Doc);
					CHECK(FormatMaterialParamAnnotation(reparsed[0]) == first);
				}

				// 未写 doc(...) 的旧注解:字段缺省为空,写出字节与旧口径一致([0,1] 与"没写范围"
				// 等价,写出时省略 —— 与 MAT-UI7a 之前的行为相同)。
				std::vector<MaterialParamDecl> legacy;
				CHECK(ParseMaterialParams("//! param Float Old = 0.25 [0,1] unit(\"x\") group(\"G\") label(\"L\")\n",
					&legacy, &error));
				CHECK(legacy.size() == 1 && legacy[0].Doc.empty());
				CHECK(FormatMaterialParamAnnotation(legacy[0])
					== "param Float Old = 0.25 unit(\"x\") group(\"G\") label(\"L\")");

				// 超长:解析与写出都截断到 kMaxMaterialParamDocBytes(256 字节)。
				const std::string longAscii(400, 'a');
				std::vector<MaterialParamDecl> longTable;
				CHECK(ParseMaterialParams("//! param Float Long = 0 doc(\"" + longAscii + "\")\n",
					&longTable, &error));
				CHECK(longTable.size() == 1);
				CHECK(longTable[0].Doc.size() == kMaxMaterialParamDocBytes);
				CHECK(longTable[0].Doc == longAscii.substr(0, kMaxMaterialParamDocBytes));

				// 多字节文本按 UTF-8 码点边界截断(100 个「中」= 300 字节),不劈半个字符。
				const auto isValidUtf8 = [](const std::string& text)
				{
					size_t index = 0;
					while (index < text.size())
					{
						const unsigned char lead = static_cast<unsigned char>(text[index]);
						size_t extra = 0;
						if (lead < 0x80u) extra = 0;
						else if ((lead & 0xE0u) == 0xC0u) extra = 1;
						else if ((lead & 0xF0u) == 0xE0u) extra = 2;
						else if ((lead & 0xF8u) == 0xF0u) extra = 3;
						else return false;
						if (index + extra >= text.size())
							return false;
						for (size_t step = 1; step <= extra; ++step)
							if ((static_cast<unsigned char>(text[index + step]) & 0xC0u) != 0x80u)
								return false;
						index += extra + 1;
					}
					return true;
				};
				std::string cjk;
				for (int index = 0; index < 100; ++index)
					cjk += "中";
				std::vector<MaterialParamDecl> cjkTable;
				CHECK(ParseMaterialParams("//! param Float Wide = 0 doc(\"" + cjk + "\")\n",
					&cjkTable, &error));
				CHECK(cjkTable.size() == 1);
				CHECK(cjkTable[0].Doc.size() <= kMaxMaterialParamDocBytes);
				CHECK(cjkTable[0].Doc == cjk.substr(0, cjkTable[0].Doc.size()));
				CHECK(isValidUtf8(cjkTable[0].Doc));
				// 写出侧同一口径:手工表里的超长 Doc 写出去 → 读回来就是截断后的文本。
				MaterialParamDecl manual;
				manual.Name = "Manual";
				manual.Type = ParamType::Float;
				manual.Default = "1";
				manual.Doc = std::string(400, 'b');
				std::vector<MaterialParamDecl> manualBack;
				CHECK(ParseMaterialParams("//! " + FormatMaterialParamAnnotation(manual) + "\n",
					&manualBack, &error));
				CHECK(manualBack.size() == 1
					&& manualBack[0].Doc.size() == kMaxMaterialParamDocBytes);

				// 非法:每个错误都给可读原因(与其它字段同一套文案)。
				const auto expectDocError = [](const std::string& text, const std::string& reasonHint)
				{
					std::vector<MaterialParamDecl> bad;
					std::string badError;
					const bool parsed = ParseMaterialParams(text, &bad, &badError);
					if (parsed || !bad.empty() || badError.find(reasonHint) == std::string::npos)
						std::printf("MaterialTests: MAT-UI7a want '%s', got: %s\n",
							reasonHint.c_str(), badError.c_str());
					CHECK(!parsed);
					CHECK(bad.empty());
					CHECK(badError.find(reasonHint) != std::string::npos);
				};
				expectDocError("//! param Float X = 1 doc(Tint)\n", "需要双引号字符串");
				expectDocError("//! param Float X = 1 doc(\"没有收尾\n", "引号没有闭合");
				expectDocError("//! param Float X = 1 doc(\"x\"\n", "缺少右括号");
				expectDocError("//! param Float X = 1 doc(\"a\") doc(\"b\")\n", "字段 doc(...) 重复");
				expectDocError("//! param Float X = 1 docs(\"x\")\n", "无法识别的注解内容");
				// 只有注释、没有 param 的行仍是错误(注释不替注解行)。
				expectDocError("//! // 只有注释\n", "缺少注解指令");
			}

			// 25. 反射(Slang-T3):从 `-reflection-json` + 同一份 SPIR-V 二进制读成员偏移/类型/绑定
			//     与"真的被读",并据此打包参数块字节。两个夹具都不需要编译器:
			//     JSON 与 Slang 实测输出同形;SPIR-V 手写(只含本解析器关心的指令)。
			{
				const std::string reflectionJson = R"JSON({
  "version": 1,
  "parameters": [
    { "name": "u_ShadowMap", "binding": { "kind": "descriptorTableSlot", "index": 3 },
      "type": { "kind": "resource", "baseShape": "texture2D" } },
    { "name": "MaterialParams", "binding": { "kind": "descriptorTableSlot", "space": 1, "index": 4 },
      "type": { "kind": "constantBuffer", "elementType": { "kind": "struct", "fields": [
        { "name": "Roughness", "type": { "kind": "scalar", "scalarType": "float32" },
          "binding": { "kind": "uniform", "offset": 0, "size": 4 } },
        { "name": "Tint", "type": { "kind": "vector", "elementCount": 4,
          "elementType": { "kind": "scalar", "scalarType": "float32" } },
          "binding": { "kind": "uniform", "offset": 16, "size": 16 } },
        { "name": "Glow", "type": { "kind": "scalar", "scalarType": "bool" },
          "binding": { "kind": "uniform", "offset": 32, "size": 4 } }
      ], "sizes": [ { "kind": "uniform", "value": 48, "alignment": 16 } ] } } },
    { "name": "Albedo", "binding": { "kind": "descriptorTableSlot", "space": 2, "index": 4 },
      "type": { "kind": "resource", "baseShape": "texture2D" } }
  ],
  "entryPoints": [ { "name": "PSMain", "stage": "fragment" } ]
})JSON";

				// 手写 SPIR-V:只放 OpDecorate / OpVariable / OpConstant / OpAccessChain。
				// 访问链指向成员 0(Roughness)与成员 2(Glow)→ Tint 未读。
				const auto emit = [](std::vector<uint32_t>& words, uint32_t opcode,
					std::vector<uint32_t> operands)
				{
					words.push_back(((static_cast<uint32_t>(operands.size()) + 1u) << 16) | opcode);
					for (const uint32_t operand : operands)
						words.push_back(operand);
				};
				std::vector<uint32_t> spirvWords = { 0x07230203u, 0x00010000u, 0u, 100u, 0u };
				emit(spirvWords, 71, { 1u, 33u, 4u });        // OpDecorate %1 Binding 4
				emit(spirvWords, 71, { 1u, 34u, 1u });        // OpDecorate %1 DescriptorSet 1
				emit(spirvWords, 71, { 5u, 33u, 4u });        // OpDecorate %5 Binding 4
				emit(spirvWords, 71, { 5u, 34u, 2u });        // OpDecorate %5 DescriptorSet 2
				emit(spirvWords, 59, { 2u, 1u, 2u });         // %1 = OpVariable %2 Uniform
				emit(spirvWords, 59, { 3u, 5u, 0u });         // %5 = OpVariable %3 UniformConstant
				emit(spirvWords, 43, { 4u, 20u, 0u });        // %20 = OpConstant %4 0
				emit(spirvWords, 43, { 4u, 21u, 2u });        // %21 = OpConstant %4 2
				emit(spirvWords, 65, { 6u, 30u, 1u, 20u });   // %30 = OpAccessChain %6 %1 %20
				emit(spirvWords, 65, { 7u, 31u, 1u, 21u });   // %31 = OpAccessChain %7 %1 %21
				std::vector<uint8_t> spirvFixture(spirvWords.size() * sizeof(uint32_t));
				std::memcpy(spirvFixture.data(), spirvWords.data(), spirvFixture.size());

				MaterialParamLayout layout;
				std::string error;
				CHECK(ReflectParamLayoutFromReflectionJson(reflectionJson, spirvFixture, &layout, &error));
				std::printf("MaterialTests: Slang reflection-json layout\n%s",
					FormatParamLayout(layout).c_str());
				// M4-S3(D1):参数块 = set 1 / binding 4(b2 让给 set0 的灯光 UBO —— GL 的 UBO
				// 单元 = binding,忽略 set)。
				CHECK(layout.CbufferSet == 1 && layout.CbufferBinding == 4);
				CHECK(layout.Fields.size() == 3);
				CHECK(layout.Fields[0].Name == "Roughness" && layout.Fields[0].ReflectedType == "float");
				CHECK(layout.Fields[0].Offset == 0 && layout.Fields[0].Size == 4);
				CHECK(layout.Fields[1].Name == "Tint" && layout.Fields[1].ReflectedType == "v4float");
				CHECK(layout.Fields[1].Offset == 16 && layout.Fields[1].Size == 16);
				CHECK(layout.Fields[2].Name == "Glow" && layout.Fields[2].ReflectedType == "uint");
				CHECK(layout.Fields[2].Type == ParamType::Bool);
				CHECK(layout.CbufferSize == 48);   // Slang 报的 uniform size(== 末端 36 → 16 字节对齐)
				CHECK(layout.UsedMembers.size() == 2);
				CHECK(layout.UsedMembers[0] == "Glow" && layout.UsedMembers[1] == "Roughness");
				CHECK(layout.Textures.size() == 1);   // 引擎自己的 u_ShadowMap(set 0)不算参数槽
				CHECK(layout.Textures[0].Name == "Albedo" && layout.Textures[0].Set == 2
					&& layout.Textures[0].Binding == 4);
				CHECK(layout.Textures[0].ReflectedType == "type.2d.image");

				// 布局 + 注解表 → 参数块字节(覆盖生效、缺项用注解默认)。
				std::vector<MaterialParamDecl> table;
				table.push_back(MaterialParamDecl { "Roughness", ParamType::Float, "0.25" });
				table.push_back(MaterialParamDecl { "Tint", ParamType::Color, "1, 1, 1, 1" });
				table.push_back(MaterialParamDecl { "Glow", ParamType::Bool, "true" });
				std::vector<MaterialParamOverride> overrides;
				overrides.push_back(MaterialParamOverride { "Roughness", "0.75" });
				overrides.push_back(MaterialParamOverride { "Glow", "false" });
				std::vector<uint8_t> bytes;
				CHECK(PackParamValues(layout, table, overrides, &bytes, &error));
				CHECK(bytes.size() == 48);
				float roughness = 0.0f;
				std::memcpy(&roughness, bytes.data(), sizeof(roughness));
				CHECK(roughness == 0.75f);
				float tint[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				std::memcpy(tint, bytes.data() + 16, sizeof(tint));
				CHECK(tint[0] == 1.0f && tint[1] == 1.0f && tint[2] == 1.0f && tint[3] == 1.0f);
        uint32_t glow = 1u;
        std::memcpy(&glow, bytes.data() + 32, sizeof(glow));
        CHECK(glow == 0u);
        std::printf("MaterialTests: M4-S2 packed %zu bytes -> Roughness=%.3f Tint=(%.2f,%.2f,%.2f,%.2f) Glow=%u\n",
          bytes.size(), static_cast<double>(roughness), static_cast<double>(tint[0]),
          static_cast<double>(tint[1]), static_cast<double>(tint[2]), static_cast<double>(tint[3]),
          glow);
      }

			// 26. `.wmat` 带 Shader + Params:默认值来自 shader、覆盖生效、未声明 → 可读警告。
			{
				CHECK(std::filesystem::exists(std::filesystem::current_path() / "CMakeLists.txt"));
				std::error_code ec;
				const std::filesystem::path directory = std::filesystem::path(WLD_ASSETPATH)
					/ "material_m4s2_tmp";
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
					return "material_m4s2_tmp/" + fileName;
				};
				MaterialLibrary& library = MaterialLibrary::Get();
				std::string error;

				writeText("glass.slang",
					"//! param Float Roughness = 0.4 [0,1] group(\"Surface\") label(\"Roughness\")\n"
					"//! param Color Tint = 1, 1, 1, 1 group(\"Surface\")\n"
					"//! param Int Steps = 2 [0,8]\n"
					"//! param Bool Glow = true\n"
					"//! param Texture2D Albedo = \"\"\n"
					"Surface Evaluate(MaterialInputs input)\n"
					"{\n"
					"    Surface surface = MakeDefaultSurface();\n"
					"    surface.Roughness = Roughness;\n"
					"    surface.BaseColor = Tint.rgb;\n"
					"    if (Glow) surface.Emissive = float3(Steps, 0.0f, 0.0f);\n"
					"    surface.BaseColor *= Albedo.Sample(input.UV).rgb;\n"
					"    return surface;\n"
					"}\n");
				writeText("mat_base.wmat",
					"FormatVersion: 2\n"
					"Shader: material_m4s2_tmp/glass.slang\n"
					"Name: \"Glass\"\n"
					"Params:\n"
					"  Roughness: 0.75\n"
					"  Albedo: \"textures/Icon.png\"\n");

				Ref<Material> material = library.Load(relative("mat_base.wmat"), &error);
				CHECK(material != nullptr);
				CHECK(error.empty());
				CHECK(material->HasShaderOverride());
				CHECK(material->ShaderPath() == relative("glass.slang"));
				CHECK(material->ShaderWarning().empty());
				CHECK(material->Params().size() == 5);
				CHECK(material->ParamOverrides().size() == 2);
				CHECK(material->HasParamOverride("Roughness"));
				CHECK(!material->HasParamOverride("Tint"));
				CHECK(material->ParamSource("Roughness") == MaterialParamSource::Local);
				CHECK(material->ParamSource("Tint") == MaterialParamSource::ShaderDefault);
				CHECK(material->ParamDefaultValue("Tint") == "1, 1, 1, 1");
				CHECK(material->ResolvedParamValue("Tint") == "1, 1, 1, 1");     // 缺参数 → shader 默认
				CHECK(material->ResolvedParamValue("Roughness") == "0.75");      // 覆盖生效
				CHECK(material->ResolvedParamValue("Albedo") == "textures/Icon.png");
				CHECK(material->ResolvedParamValue("Steps") == "2");
				CHECK(!material->ParamMatchesDefault("Roughness"));
				CHECK(material->ParamWarnings().empty());
				CHECK(library.GetLoadWarning(relative("mat_base.wmat")).empty());

				// "与默认相同"的三态判定 + 值归一(SetParamOverride 会按声明类型规范化)。
				material->SetParamOverride("Steps", "2");
				CHECK(material->ParamMatchesDefault("Steps"));
				material->SetParamOverride("Tint", "0.5,0.5,0.5");
				CHECK(material->HasParamOverride("Tint"));
				CHECK(material->ResolvedParamValue("Tint") == "0.5, 0.5, 0.5, 1");
				CHECK(!material->ParamMatchesDefault("Tint"));

				// 未声明参数:载入不算失败,给可读警告,值保留在文件里。
				writeText("mat_undeclared.wmat",
					"FormatVersion: 2\n"
					"Shader: material_m4s2_tmp/glass.slang\n"
					"Params:\n"
					"  Nope: 1\n");
				std::string undeclaredWarning;
				Ref<Material> undeclared = library.Load(relative("mat_undeclared.wmat"), &undeclaredWarning);
				CHECK(undeclared != nullptr);
				CHECK(undeclaredWarning.find("没有声明") != std::string::npos);
				CHECK(undeclaredWarning.find("Nope") != std::string::npos);
				CHECK(undeclared->ParamWarnings().size() == 1);
				CHECK(library.GetLoadWarning(relative("mat_undeclared.wmat")).find("Nope")
					!= std::string::npos);
				CHECK(library.Save(undeclared, relative("mat_undeclared.wmat"), &error));
				CHECK(readText("mat_undeclared.wmat").find("Nope: 1") != std::string::npos);

				// 值类型不符:也不失败,警告里带原因,写回时原样保留。
				writeText("mat_badvalue.wmat",
					"FormatVersion: 2\n"
					"Shader: material_m4s2_tmp/glass.slang\n"
					"Params:\n"
					"  Roughness: 0.5, 0.5\n");
				std::string badValueWarning;
				Ref<Material> badValue = library.Load(relative("mat_badvalue.wmat"), &badValueWarning);
				CHECK(badValue != nullptr);
				CHECK(badValueWarning.find("不符") != std::string::npos);
				CHECK(badValue->ResolvedParamValue("Roughness") == "0.5, 0.5");
				CHECK(library.Save(badValue, relative("mat_badvalue.wmat"), &error));
				CHECK(readText("mat_badvalue.wmat").find("Roughness: \"0.5, 0.5\"") != std::string::npos);

				// shader 读不到:材质仍可用,警告指出 shader,参数默认值不可用。
				writeText("mat_missing_shader.wmat",
					"FormatVersion: 2\n"
					"Shader: material_m4s2_tmp/__missing__.slang\n"
					"Params:\n"
					"  Roughness: 0.3\n");
				std::string missingWarning;
				Ref<Material> missing = library.Load(relative("mat_missing_shader.wmat"), &missingWarning);
				CHECK(missing != nullptr);
				CHECK(missingWarning.find("读不到") != std::string::npos);
				CHECK(missing->Params().empty());
				CHECK(missing->ResolvedParamValue("Roughness") == "0.3");

				// 注解坏的 shader:同样只给警告,不拖垮材质。
				writeText("broken.slang", "//! param Float3 X = 1\n");
				writeText("mat_broken_shader.wmat",
					"FormatVersion: 2\n"
					"Shader: material_m4s2_tmp/broken.slang\n");
				std::string brokenWarning;
				Ref<Material> broken = library.Load(relative("mat_broken_shader.wmat"), &brokenWarning);
				CHECK(broken != nullptr);
				CHECK(brokenWarning.find("注解参数表解析失败") != std::string::npos);
				CHECK(brokenWarning.find("1:11:") != std::string::npos);

				// Slang-B1sk:`.hlsl` 不再是材质着色器资产类型 —— 旧 `.wmat` 里的
				// `Shader: *.hlsl` 引用**没有**任何兼容/迁移提示,只剩通用的
				// "读不到那份源"警告(参数表空,覆盖值保留)。新写路径只有 `.slang`。
				writeText("mat_hlslext_shader.wmat",
					"FormatVersion: 2\n"
					"Shader: material_m4s2_tmp/hlslext_glass.hlsl\n"
					"Params:\n"
					"  Roughness: 0.1\n");
				std::string hslExtWarning;
				Ref<Material> hslExt = library.Load(relative("mat_hlslext_shader.wmat"), &hslExtWarning);
				CHECK(hslExt != nullptr);                                  // .wmat 本身照样加载
				CHECK(hslExt->HasShaderOverride());
				CHECK(hslExt->ShaderPath() == relative("hlslext_glass.hlsl"));
				CHECK(hslExt->Params().empty());                           // 源读不到 → 参数表空
				CHECK(hslExt->ShaderWarning().find("读不到") != std::string::npos);
				CHECK(hslExt->ShaderWarning().find(".hlsl") != std::string::npos);
				CHECK(hslExt->ShaderWarning().find("legacy") == std::string::npos);
				CHECK(hslExt->ShaderWarning().find("migrate") == std::string::npos);
				// Load 警告 = shader 警告 + 参数警告拼接(未声明 override 也会进这里),
				// 所以这里是"包含"(旧用例只比 shader 警告,是因为当时没有参数警告)。
				CHECK(hslExtWarning.find(hslExt->ShaderWarning()) != std::string::npos);
				CHECK(library.GetLoadWarning(relative("mat_hlslext_shader.wmat"))
					.find(hslExt->ShaderWarning()) != std::string::npos);
				std::printf("[Slang-B1sk] .hlsl material reference (no migration hint): %s\n",
					hslExt->ShaderWarning().c_str());

				// 父级继承:Shader 与注解表跟随父级,参数生效值 = 本文件 > 父级 > shader 默认。
				writeText("mat_parent.wmat",
					"FormatVersion: 2\n"
					"Shader: material_m4s2_tmp/glass.slang\n"
					"Name: \"Parent\"\n"
					"Params:\n"
					"  Roughness: 0.5\n");
				writeText("mat_child.wmat",
					"FormatVersion: 2\n"
					"Parent: material_m4s2_tmp/mat_parent.wmat\n"
					"Name: \"Child\"\n"
					"Params:\n"
					"  Tint: 0.25, 0.25, 0.25, 1\n");
				Ref<Material> child = library.Load(relative("mat_child.wmat"), &error);
				CHECK(child != nullptr);
				CHECK(error.empty());
				CHECK(!child->HasShaderOverride());
				CHECK(child->ShaderPath() == relative("glass.slang"));   // 继承父级
				CHECK(child->Params().size() == 5);                     // 注解表也继承
				CHECK(!child->HasParamOverride("Roughness"));           // 本文件没写
				CHECK(child->ParamSource("Roughness") == MaterialParamSource::Parent);
				CHECK(child->ResolvedParamValue("Roughness") == "0.5");
				CHECK(child->ParamSource("Tint") == MaterialParamSource::Local);
				CHECK(child->ResolvedParamValue("Tint") == "0.25, 0.25, 0.25, 1");
				CHECK(child->ParamSource("Steps") == MaterialParamSource::ShaderDefault);

				// 27. 保存只写覆盖项(逐行断言):未覆盖的字段/参数一行都不出现。
				CHECK(library.Save(child, relative("mat_child_saved.wmat"), &error));
				const std::string saved = readText("mat_child_saved.wmat");
				CHECK(saved == "FormatVersion: 2\n"
					"Parent: material_m4s2_tmp/mat_parent.wmat\n"
					"Name: \"Child\"\n"
					"Params:\n"
					"  Tint: [0.25, 0.25, 0.25, 1]\n");
				CHECK(saved.find("Shader:") == std::string::npos);        // 没覆盖 → 不写(继承父级)
				CHECK(saved.find("Roughness") == std::string::npos);
				CHECK(saved.find("Metallic") == std::string::npos);

				// 写了 Shader 的本文件:写出带 Shader + 只带自己的覆盖项。
				material->SetParamOverride("Steps", "5");
				CHECK(library.Save(material, relative("mat_base_saved.wmat"), &error));
				const std::string savedBase = readText("mat_base_saved.wmat");
				CHECK(savedBase.find("FormatVersion: 2\n") == 0);
				CHECK(savedBase.find("Shader: material_m4s2_tmp/glass.slang\n") != std::string::npos);
				CHECK(savedBase.find("  Roughness: 0.75\n") != std::string::npos);
				CHECK(savedBase.find("  Albedo: \"textures/Icon.png\"\n") != std::string::npos);
				CHECK(savedBase.find("  Tint: [0.5, 0.5, 0.5, 1]\n") != std::string::npos);
				CHECK(savedBase.find("  Steps: 5\n") != std::string::npos);
				CHECK(savedBase.find("Glow") == std::string::npos);       // 没写覆盖
				Ref<Material> reopened = library.Load(relative("mat_base_saved.wmat"), &error);
				CHECK(reopened != nullptr);
				CHECK(reopened->ParamOverrides().size() == 4);
				CHECK(reopened->ResolvedParamValue("Steps") == "5");
				CHECK(reopened->ResolvedParamValue("Glow") == "true");

				// 回退覆盖:文件里那一行消失(值回到 shader 默认)。
				reopened->RevertParam("Steps");
				reopened->RevertParam("Roughness");
				CHECK(library.Save(reopened, relative("mat_base_saved.wmat"), &error));
				const std::string reverted = readText("mat_base_saved.wmat");
				CHECK(reverted.find("  Steps:") == std::string::npos);
				CHECK(reverted.find("  Roughness:") == std::string::npos);
				CHECK(reopened->ResolvedParamValue("Roughness") == "0.4");   // shader 默认

				// Shader 覆盖可以回退(回到父级/引擎默认);没有父级时 = 不做表面函数。
				material->RevertShader();
				CHECK(!material->HasShaderOverride());
				CHECK(material->ShaderPath().empty());
				CHECK(material->Params().empty());

				// 28. M4-S3:材质着色器(`.slang`)内容进 `.wmat` 指纹;源变化被资产热重载看见
				//     (报告条目 + 引用它的材质失效 → 参数表刷新、Revision 前进)。
				{
					writeText("m4s3_probe.slang",
						"//! param Float Roughness = 0.4 [0,1]\n"
						"//! param Int Steps = 2 [0,8]\n"
						"Surface Evaluate(MaterialInputs input)\n"
						"{\n"
						"    Surface surface = MakeDefaultSurface();\n"
						"    surface.Roughness = Roughness;\n"
						"    surface.Emissive = float3(Steps, 0.0f, 0.0f);\n"
						"    return surface;\n"
						"}\n");
					writeText("m4s3_probe.wmat",
						"FormatVersion: 2\n"
						"Shader: material_m4s2_tmp/m4s3_probe.slang\n"
						"Name: \"Probe\"\n"
						"Params:\n"
						"  Roughness: 0.6\n");

					Ref<Material> probe = library.Load(relative("m4s3_probe.wmat"), &error);
					CHECK(probe != nullptr);
					CHECK(probe->Params().size() == 2);
					// 表面管线键 = 规范化的 shader 路径;预览用的覆盖键是 `<路径>#preview`。
					CHECK(probe->SurfaceKey() == relative("m4s3_probe.slang"));
					probe->SetSurfaceKeyOverride(relative("m4s3_probe.slang") + "#preview");
					CHECK(probe->SurfaceKey() == relative("m4s3_probe.slang") + "#preview");
					probe->SetSurfaceKeyOverride(std::string());
					CHECK(probe->SurfaceKey() == relative("m4s3_probe.slang"));
					CHECK(probe->SurfaceKeyOverride().empty());

					const AssetFingerprint shaderBefore = FingerprintAsset(relative("m4s3_probe.slang"));
					const AssetFingerprint materialBefore = FingerprintAsset(relative("m4s3_probe.wmat"));
					CHECK(shaderBefore.Exists && shaderBefore.FromContent);
					CHECK(materialBefore.Exists && materialBefore.FromContent);

					// 监听基线必须在改文件之前建立(Watch() 首次登记不报告)。
					AssetHotReloadReport baseline;
					library.PollAssetChanges(0.0, baseline);
					CHECK(!baseline.Any());

				const uint32_t revisionBefore = probe->GetRevision();
				const size_t paramsBefore = probe->Params().size();
				writeText("m4s3_probe.slang",
						"//! param Float Roughness = 0.4 [0,1]\n"
						"Surface Evaluate(MaterialInputs input)\n"
						"{\n"
						"    Surface surface = MakeDefaultSurface();\n"
						"    surface.Roughness = Roughness * 0.5f;\n"
						"    return surface;\n"
						"}\n");

					// 指纹:源自己变 + 引用它的 .wmat 一起变(父级链同款口径)。
					const AssetFingerprint shaderAfter = FingerprintAsset(relative("m4s3_probe.slang"));
					const AssetFingerprint materialAfter = FingerprintAsset(relative("m4s3_probe.wmat"));
					CHECK(shaderAfter.Value != shaderBefore.Value);
					CHECK(materialAfter.Value != materialBefore.Value);

					const char* hotReloadSwitch = std::getenv("WLD_ASSET_HOTRELOAD");
					const bool hotReloadEnabled = !(hotReloadSwitch && *hotReloadSwitch
						&& std::string(hotReloadSwitch) == "0");
					if (hotReloadEnabled)
					{
						AssetHotReloadReport observed;
						library.PollAssetChanges(0.0, observed);       // 看到新内容,debounce 未到
						CHECK(observed.ChangedShaders.empty());
						library.PollAssetChanges(0.2, observed);       // 稳定 0.2s ≥ 0.15s → 报告
						CHECK(observed.ChangedShaders.size() == 1);
						CHECK(observed.ChangedShaders[0] == relative("m4s3_probe.slang"));
					// 引用它的材质失效:参数表刷新(Steps 注解已删)+ Revision 前进。
					CHECK(probe->Params().size() == 1);
					CHECK(probe->GetRevision() > revisionBefore);
					std::printf("MaterialTests: M4-S3 shader fingerprint %016llx -> %016llx, "
						"material fingerprint %016llx -> %016llx, revision %u -> %u (params %zu -> %zu)\n",
						static_cast<unsigned long long>(shaderBefore.Value),
						static_cast<unsigned long long>(shaderAfter.Value),
						static_cast<unsigned long long>(materialBefore.Value),
						static_cast<unsigned long long>(materialAfter.Value),
						revisionBefore, probe->GetRevision(), paramsBefore, probe->Params().size());
				}
			}

			library.Shutdown();
			std::filesystem::remove_all(directory, ec);
			}
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
