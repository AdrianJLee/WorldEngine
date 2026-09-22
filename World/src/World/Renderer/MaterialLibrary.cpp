#include "wldpch.h"

#include "World/Renderer/MaterialLibrary.h"

#include "World/Core/Log.h"
#include "World/Renderer/MaterialTextureCache.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <unordered_set>

namespace World
{
	namespace
	{
		// 磁盘位置:**内容根**(Game/assets),与 MaterialIO::ReadFileText 同一约定。
		// P4-U12:删掉"再试 Game/ 旧布局"的第二候选 —— 内容根只有一个。
		std::filesystem::path ResolveMaterialDiskPath(const std::string& path)
		{
			std::error_code ec;
			const std::filesystem::path candidate =
				std::filesystem::path(std::string(WLD_GAME_DIR)) / "assets" / path;
			if (std::filesystem::exists(candidate, ec))
				return candidate;
			return {};
		}

		std::filesystem::file_time_type FileWriteTime(const std::string& path)
		{
			std::error_code ec;
			const std::filesystem::path target = ResolveMaterialDiskPath(path);
			if (target.empty())
				return std::filesystem::file_time_type::min();
			const auto time = std::filesystem::last_write_time(target, ec);
			return ec ? std::filesystem::file_time_type::min() : time;
		}

		bool AssetHotReloadEnabled()
		{
			const char* value = std::getenv("WLD_ASSET_HOTRELOAD");
			return !(value && *value && std::string(value) == "0");
		}

		bool AssetHotReloadTraceEnabled()
		{
			const char* value = std::getenv("WLD_ASSET_HOTRELOAD_TRACE");
			return value && *value && std::string(value) != "0";
		}

		void PrintHotReloadTrace(const char* format, const std::string& path)
		{
			if (AssetHotReloadTraceEnabled())
				WLD_CORE_INFO("[asset-hot-reload] {0} {1}", format, path);
		}
	}

	MaterialLibrary& MaterialLibrary::Get()
	{
		// 刻意用堆分配且不析构:与 Renderer 的钩子表同风格,避免静态析构顺序问题。
		static MaterialLibrary* instance = new MaterialLibrary();
		return *instance;
	}

	void MaterialLibrary::Shutdown()
	{
		MaterialLibrary& library = Get();
		library.m_Cache.clear();
		library.m_Warnings.clear();
		library.m_ResolvingStack.clear();
	}

	std::string MaterialLibrary::NormalizePath(const std::string& path)
	{
		// 同一套规范化只有一处实现(材质解析 MaterialIO::NormalizePath);
		// 老入口保留,既有调用点不用改。
		return MaterialIO::NormalizePath(path);
	}

	MaterialLibrary::Resolution MaterialLibrary::Resolve(const std::string& rawKey)
	{
		Resolution result;
		const std::string key = NormalizePath(rawKey);
		if (key.empty())
		{
			result.Error = "路径为空";
			return result;
		}
		const auto cached = m_Cache.find(key);
		if (cached != m_Cache.end())
		{
			result.Instance = cached->second;
			result.Warning = GetLoadWarning(key);
			return result;
		}

		// 循环引用:当前解析栈里已经有这个路径 → 拒绝 + 可读链路(不崩、不死循环)。
		const auto onStack = std::find(m_ResolvingStack.begin(), m_ResolvingStack.end(), key);
		if (onStack != m_ResolvingStack.end())
		{
			std::string chain;
			for (auto it = onStack; it != m_ResolvingStack.end(); ++it)
				chain.append(*it).append(" -> ");
			chain.append(key);
			result.Error = "材质父级循环引用: " + chain;
			result.ChainError = true;
			return result;
		}
		// 深度上限(方案:递归,深度上限 8):父级链最多 8 级,第 9 级同样是结构错误,
		// 拒绝而不是"猜一个"。栈里已有的层数 = 当前节点的父级层数。
		if (m_ResolvingStack.size() > kMaxParentDepth)
		{
			std::string chain;
			for (const std::string& entry : m_ResolvingStack)
				chain.append(entry).append(" -> ");
			chain.append(key);
			result.Error = "材质父级链超过 " + std::to_string(kMaxParentDepth) + " 级: " + chain;
			result.ChainError = true;
			return result;
		}

		std::string text;
		if (!MaterialIO::ReadFileText(key, text))
		{
			result.Error = "找不到材质文件 " + key;
			return result;
		}

		MaterialDocument document;
		std::string parseError;
		const MaterialLoadResult parsed = MaterialIO::ParseDocument(text, document, &parseError);
		if (!parsed.Success)
		{
			result.Error = parsed.Error;
			return result;
		}

		// 父级链:M3 起 .wmat 只写覆盖字段,其余继承父级(没有父级 = 引擎内置默认)。
		Ref<Material> parent;
		std::string parentError;
		{
			// RAII:无论中途成功/失败(含异常)都把当前路径出栈,否则后续加载会误报循环。
			struct StackGuard
			{
				explicit StackGuard(std::vector<std::string>& stack) : Stack(stack) {}
				~StackGuard() { if (!Stack.empty()) Stack.pop_back(); }
				std::vector<std::string>& Stack;
			};
			m_ResolvingStack.push_back(key);
			const StackGuard guard(m_ResolvingStack);
			if (!document.ParentPath.empty())
			{
				Resolution parentResult = Resolve(document.ParentPath);
				if (parentResult.ChainError)
				{
					// 循环/超深:整条链都是坏的 → 向上拒绝(不退化成默认,否则问题被藏起来)。
					result.Error = parentResult.Error;
					result.ChainError = true;
					return result;
				}
				if (parentResult.Instance)
					parent = parentResult.Instance;
				else
					parentError = parentResult.Error;
			}
		}

		const MaterialDesc desc = MaterialIO::MergeDocument(document, parent ? &parent->GetDesc() : nullptr);
		Ref<Material> material(new Material(desc, key));
		material->m_ParentPath = document.ParentPath;
		material->m_Overrides = document.Overridden;
		material->m_Parent = parent;
		material->m_ParentWarning = parentError;
		material->m_FileTime = FileWriteTime(key);
		m_Cache.emplace(key, material);

		std::string warning = parsed.Error;
		if (!parentError.empty())
		{
			if (!warning.empty())
				warning.append("; ");
			warning.append("父级 '").append(document.ParentPath).append("' 不可用: ")
				.append(parentError).append("; 已退化为引擎内置默认");
			WLD_CORE_WARN("[material] '{0}':父级 '{1}' 不可用({2}),已退化为引擎内置默认",
				key, document.ParentPath, parentError);
		}
		if (warning.empty())
			m_Warnings.erase(key);
		else
			m_Warnings[key] = warning;

		result.Instance = material;
		result.Warning = warning;
		return result;
	}

	void MaterialLibrary::AdoptResolved(Material& target, const Material& source)
	{
		const bool valuesChanged = !(target.m_Desc == source.m_Desc);
		target.m_Desc = source.m_Desc;
		target.m_ParentPath = source.m_ParentPath;
		target.m_Overrides = source.m_Overrides;
		target.m_Parent = source.m_Parent;
		target.m_ParentWarning = source.m_ParentWarning;
		target.m_FileTime = source.m_FileTime;
		target.m_Dirty = false;
		if (valuesChanged)
			target.BumpRevision();   // 值真的变了才前进(与 M3 前 SetDesc 的口径一致)
	}

	Ref<Material> MaterialLibrary::Load(const std::string& path, std::string* error)
	{
		Resolution resolution = Resolve(path);
		if (!resolution.Instance)
		{
			if (error) *error = resolution.Error;
			return nullptr;
		}
		if (error) *error = resolution.Warning;   // 成功但带警告(夹紧 / 父级退化)
		return resolution.Instance;
	}

	Ref<Material> MaterialLibrary::CreateDefault(const std::string& name)
	{
		MaterialDesc desc;
		desc.Name = name;
		Ref<Material> material(new Material(desc, std::string()));
		// Name 是这份新实例自己的覆盖字段(其余字段继承引擎内置默认)。
		material->m_Overrides.Set(MaterialField::Name);
		material->MarkDirty(true);
		return material;
	}

	Ref<Material> MaterialLibrary::CreateInstance(const std::string& parentPath,
		const std::string& name, std::string* error)
	{
		const std::string key = NormalizePath(parentPath);
		Ref<Material> parent;
		std::string warning;
		if (!key.empty())
		{
			Resolution parentResult = Resolve(key);
			if (parentResult.ChainError)
			{
				if (error) *error = parentResult.Error;
				return nullptr;   // 链结构错误:不造出"看起来能用"的实例
			}
			parent = parentResult.Instance;
			if (parent)
			{
				warning = parentResult.Warning;
			}
			else
			{
				warning = "父级 '";
				warning.append(key).append("' 不可用: ").append(parentResult.Error)
					.append("; 已退化为引擎内置默认");
				WLD_CORE_WARN("[material] 新建实例的父级 '{0}' 不可用({1}),已退化为引擎内置默认",
					key, parentResult.Error);
			}
		}

		MaterialDesc desc = parent ? parent->GetDesc() : MaterialIO::DefaultMaterialDesc();
		MaterialFieldSet overrides;
		if (!name.empty())
		{
			desc.Name = name;
			overrides.Set(MaterialField::Name);
		}

		Ref<Material> material(new Material(desc, std::string()));
		material->m_ParentPath = key;
		material->m_Overrides = overrides;
		material->m_Parent = parent;
		material->m_ParentWarning = parent ? std::string() : warning;
		material->MarkDirty(true);
		if (error) *error = warning;
		return material;
	}

	bool MaterialLibrary::Save(const Ref<Material>& material, const std::string& path, std::string* error)
	{
		if (!material)
		{
			if (error) *error = "材质为空";
			return false;
		}
		std::string key = NormalizePath(path.empty() ? material->GetPath() : path);
		if (key.empty())
		{
			if (error) *error = "保存路径为空(新建材质需要另存为)";
			return false;
		}
		if (key.size() < 5 || key.substr(key.size() - 5) != ".wmat")
			key.append(".wmat");

		// M3:写出 = 覆盖字段 + Parent(没有父级时不写 Parent);
		// 全字段 + 无父级的老形态仍然按 v1 写,与 M3 前逐字节一致。
		MaterialDocument document;
		document.ParentPath = material->m_ParentPath;
		document.Values = material->GetDesc();
		document.Overridden = material->m_Overrides;
		const std::string text = MaterialIO::SerializeDocument(document);
		if (!MaterialIO::WriteFileText(key, text, error))
			return false;

		// 回读校验:确保写出的文件能被自己解析,而且覆盖集 / 父级 / 合并后的值都与内存态一致。
		// U23:比较口径 = MaterialIO::EquivalentForSave(浮点按 1e-6 容差、其余字段严格相等)。
		// 逐位相等会把"拖一下滑杆"的正常浮点尾差判成写入失败(见 Material.cpp FormatFloat 的说明);
		// 容差仍然拒绝类型/字符串/枚举写坏与超过 1e-6 的数值错误。
		// 父级值用内存里的父实例(保存不该受磁盘上父级文件当前状态影响)。
		MaterialDocument verifyDocument;
		std::string verifyError;
		const MaterialDesc* parentDesc = material->m_Parent ? &material->m_Parent->GetDesc() : nullptr;
		if (!MaterialIO::ParseDocument(text, verifyDocument, &verifyError).Success
			|| verifyDocument.ParentPath != document.ParentPath
			|| verifyDocument.Overridden != document.Overridden
			|| !MaterialIO::EquivalentForSave(
				MaterialIO::MergeDocument(verifyDocument, parentDesc), material->GetDesc()))
		{
			if (error) *error = "写入校验失败: " + verifyError;
			return false;
		}

		// 换路径(另存为):旧缓存键让位,实例本身保持同一性(编辑器引用不失效)。
		const std::string previous = material->GetPath();
		if (!previous.empty() && previous != key)
		{
			const auto oldEntry = m_Cache.find(previous);
			if (oldEntry != m_Cache.end() && oldEntry->second == material)
				m_Cache.erase(oldEntry);
			m_Warnings.erase(previous);
		}
		material->SetPath(key);
		material->MarkDirty(false);
		material->InvalidateTextures();   // 磁盘内容变化 → 贴图/GPU 侧重建
		material->m_FileTime = FileWriteTime(key);
		m_Cache[key] = material;
		m_Warnings.erase(key);
		return true;
	}

	bool MaterialLibrary::Reload(const std::string& path, std::string* error)
	{
		const std::string key = NormalizePath(path);
		const auto cached = m_Cache.find(key);
		const Ref<Material> existing = cached != m_Cache.end() ? cached->second : nullptr;
		// 强制重走一遍父级链(父级命中缓存时照旧复用),再原地更新已有实例。
		// 注意:摘掉的是自己的缓存条目 —— 别的材质仍持有同一父实例的 Ref,实例同一性不受影响。
		m_Cache.erase(key);
		Resolution resolution = Resolve(key);
		if (!resolution.Instance)
		{
			// 坏文件 / 坏链:保留旧内存态(绝不因为一次读取失败丢掉已加载材质)。
			if (existing)
				m_Cache[key] = existing;
			if (error) *error = resolution.Error;
			return false;
		}

		if (existing)
		{
			// 原地更新:保留实例(编辑器/渲染侧的 Ref 不失效),值变化时 Revision 让 GPU 侧重建。
			AdoptResolved(*existing, *resolution.Instance);
			m_Cache[key] = existing;
		}
		if (error) *error = resolution.Warning;
		return true;
	}

	bool MaterialLibrary::IsFileNewer(const Material& material) const
	{
		// M3:父级链上任何一份文件比它自己的内存态新,子材质也算"被外部改过"。
		for (const Material* current = &material; current != nullptr; current = current->ResolvedParent().get())
		{
			if (current->GetPath().empty())
				continue;
			const auto time = FileWriteTime(current->GetPath());
			if (time == std::filesystem::file_time_type::min())
				continue;
			if (time > current->m_FileTime)
				return true;
		}
		return false;
	}

	void MaterialLibrary::PollAssetChanges(double deltaSeconds, AssetHotReloadReport& report)
	{
		if (!AssetHotReloadEnabled())
		{
			// 整体关闭:不建立也不推进监听(重新开启时重新建立基线)。
			m_MaterialWatch.Clear();
			m_TextureWatch.Clear();
			return;
		}

		// ---- 监听集合同步:材质 = 当前缓存;贴图 = 缓存材质引用的 Albedo/Normal ----
		std::vector<std::string> wantedMaterials;
		wantedMaterials.reserve(m_Cache.size());
		std::vector<std::string> wantedTextures;
		const auto addTexture = [&wantedTextures](const std::string& path)
		{
			if (path.empty())
				return;
			const std::string normalized = MaterialLibrary::NormalizePath(path);
			if (normalized.empty())
				return;
			if (std::find(wantedTextures.begin(), wantedTextures.end(), normalized) == wantedTextures.end())
				wantedTextures.push_back(normalized);
		};
		for (const auto& [key, material] : m_Cache)
		{
			wantedMaterials.push_back(key);
			if (!material)
				continue;
			addTexture(material->GetDesc().AlbedoTexture);
			addTexture(material->GetDesc().NormalTexture);
		}
		std::sort(wantedMaterials.begin(), wantedMaterials.end());
		std::sort(wantedTextures.begin(), wantedTextures.end());

		const std::vector<std::string> watchedMaterials = m_MaterialWatch.WatchedPaths();
		for (const std::string& path : wantedMaterials)
			if (std::find(watchedMaterials.begin(), watchedMaterials.end(), path) == watchedMaterials.end())
				m_MaterialWatch.Watch(path);
		for (const std::string& path : watchedMaterials)
			if (std::find(wantedMaterials.begin(), wantedMaterials.end(), path) == wantedMaterials.end())
				m_MaterialWatch.Unwatch(path);

		const std::vector<std::string> watchedTextures = m_TextureWatch.WatchedPaths();
		for (const std::string& path : wantedTextures)
			if (std::find(watchedTextures.begin(), watchedTextures.end(), path) == watchedTextures.end())
				m_TextureWatch.Watch(path);
		for (const std::string& path : watchedTextures)
			if (std::find(wantedTextures.begin(), wantedTextures.end(), path) == wantedTextures.end())
				m_TextureWatch.Unwatch(path);
		if (AssetHotReloadTraceEnabled())
			WLD_CORE_INFO("[asset-hot-reload] watching {0} material(s), {1} texture(s)",
				wantedMaterials.size(), wantedTextures.size());

		// ---- 材质:.wmat 内容变化(已过 debounce)→ clean 原地重载 / dirty 只报告 ----
		// M3:子材质的指纹含解析后的父级链(见 FingerprintAsset),所以父级改动会让子材质
		// 一起出现在这一批里。按"父级深度升序"重载,保证子级读到的是本轮已经刷新的父级值
		// (链式继承当轮生效,不用等下一拍)。
		std::vector<std::string> changedMaterials = m_MaterialWatch.Poll(deltaSeconds);
		if (changedMaterials.size() > 1)
		{
			const auto depthOf = [this](const std::string& path)
			{
				const auto entry = m_Cache.find(path);
				if (entry == m_Cache.end() || !entry->second)
					return 0;
				int depth = 0;
				for (Ref<Material> parent = entry->second->ResolvedParent();
					parent && depth <= static_cast<int>(kMaxParentDepth); parent = parent->ResolvedParent())
					++depth;
				return depth;
			};
			std::stable_sort(changedMaterials.begin(), changedMaterials.end(),
				[&depthOf](const std::string& a, const std::string& b) { return depthOf(a) < depthOf(b); });
		}
		for (const std::string& path : changedMaterials)
		{
			const auto entry = m_Cache.find(path);
			if (entry == m_Cache.end() || !entry->second)
				continue;   // 已不在缓存(轮询与缓存同步之间消失)
			if (entry->second->IsDirty())
			{
				// 有未保存修改:只报告,绝不覆盖(面板的脏标记由编辑器维护)。
				report.SkippedDirtyMaterials.push_back(path);
				PrintHotReloadTrace("skip dirty material", path);
				continue;
			}
			std::string error;
			if (Reload(path, &error))
			{
				report.ReloadedMaterials.push_back(path);
				PrintHotReloadTrace("reloaded material", path);
			}
			else
			{
				// 读取/解析失败:保留旧内存态(Reload 失败不改实例)。
				report.FailedMaterials.push_back(AssetReloadFailure { path, error });
				PrintHotReloadTrace("failed material", path);
			}
		}

		// ---- 贴图:内容变化 → 清 s:/l: 缓存 + 引用方 Revision 前进(旧句柄延迟释放) ----
		const std::vector<std::string> changedTextures = m_TextureWatch.Poll(deltaSeconds);
		if (!changedTextures.empty())
		{
			std::unordered_set<std::string> changed(changedTextures.begin(), changedTextures.end());
			for (const std::string& path : changedTextures)
			{
				const std::string normalized = MaterialLibrary::NormalizePath(path);
				// 清 s:/l: 两份;旧句柄由缓存内部按 Renderer::QueueRelease 延迟释放
				// (GL 立即、Vulkan 三帧/fence 后;无设备时安全 no-op)。
				MaterialTextureCache::Get().Invalidate(normalized);
			}

			for (const auto& [key, material] : m_Cache)
			{
				if (!material)
					continue;
				const MaterialDesc& desc = material->GetDesc();
				const bool albedoChanged = !desc.AlbedoTexture.empty()
					&& changed.count(MaterialLibrary::NormalizePath(desc.AlbedoTexture)) != 0;
				const bool normalChanged = !desc.NormalTexture.empty()
					&& changed.count(MaterialLibrary::NormalizePath(desc.NormalTexture)) != 0;
				if (albedoChanged || normalChanged)
					material->InvalidateTextures();
			}
			for (const std::string& normalized : changedTextures)
			{
				report.InvalidatedTextures.push_back(normalized);
				PrintHotReloadTrace("invalidated texture", normalized);
			}
		}
	}

	std::string MaterialLibrary::GetLoadWarning(const std::string& path) const
	{
		const auto it = m_Warnings.find(NormalizePath(path));
		return it == m_Warnings.end() ? std::string() : it->second;
	}

	std::vector<std::string> MaterialLibrary::ScanMaterials() const
	{
		std::vector<std::string> paths;
		std::error_code ec;
		// 内容根 = Game/assets(与材质路径的书写约定一致);遍历失败时返回空列表。
		const std::filesystem::path root = std::filesystem::path(std::string(WLD_GAME_DIR)) / "assets";
		if (!std::filesystem::exists(root, ec))
			return paths;
		for (const std::filesystem::directory_entry& entry :
			std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec))
		{
			if (!entry.is_regular_file(ec) || entry.path().extension() != ".wmat")
				continue;
			const std::filesystem::path relative = std::filesystem::relative(entry.path(), root, ec);
			if (!ec)
				paths.push_back(NormalizePath(relative.generic_string()));
		}
		std::sort(paths.begin(), paths.end());
		return paths;
	}
}
