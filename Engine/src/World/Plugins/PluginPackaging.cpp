#include "wldpch.h"
#include "World/Plugins/PluginPackaging.h"

#include "World/Core/Log.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace World::Plugins
{
	namespace
	{
		namespace fs = std::filesystem;

		std::string JoinIds(const std::vector<std::string>& ids)
		{
			if (ids.empty())
				return "none";
			std::string text;
			for (const std::string& id : ids)
			{
				if (!text.empty())
					text += ",";
				text += id;
			}
			return text;
		}

		std::string ToLower(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(),
				[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			return text;
		}

		std::string Trim(std::string text)
		{
			const size_t first = text.find_first_not_of(" \t\r\n");
			if (first == std::string::npos)
				return {};
			const size_t last = text.find_last_not_of(" \t\r\n");
			return text.substr(first, last - first + 1);
		}

		bool Contains(const std::vector<std::string>& values, const std::string& value)
		{
			return std::find(values.begin(), values.end(), value) != values.end();
		}

		// 内容里出现的一个"插件提供面"的引用(硬门的最小记录单位)。
		struct ReferenceHit
		{
			PluginContributionFace Face = PluginContributionFace::Component;
			std::string Id;
			std::string Referrer;   // 引用者(内容根相对路径;场景/脚本带定位)
			std::string PluginId;   // 声明该 id 的插件
		};

		// 由所有**已发现**插件的 `contributes:` 拼出的引用索引(闭包外的插件也在内 ——
		// 正是"引用了没启用的插件"能被判定的原因)。
		struct ProvideIndex
		{
			std::map<std::string, std::string> Components;
			std::map<std::string, std::string> AssetTypes;
			std::map<std::string, std::string> AssetTypeExtensions;   // 小写、带点
			std::map<std::string, std::string> Importers;
			std::map<std::string, std::string> ImporterExtensions;    // 小写、带点
			std::map<std::string, std::string> ScriptNamespaces;
		};

		// 同 id 二次声明:先到者生效,后者记警告 —— 与"同能力多提供者 = 项目插件 > 引擎插件"
		// 同口径,且绝不静默(BuildProvideIndex 因此先放项目插件、再放引擎插件)。
		void InsertProvider(std::map<std::string, std::string>& table, const std::string& key,
			const std::string& pluginId, const char* face, std::vector<std::string>* warnings)
		{
			const auto found = table.find(key);
			if (found == table.end())
			{
				table.emplace(key, pluginId);
				return;
			}
			if (found->second == pluginId)
				return;   // 同一插件重复声明同一条 = 无害,不记
			if (warnings)
				warnings->push_back(std::string("contribution conflict: ") + face + " '" + key
					+ "' is declared by '" + found->second + "' and '" + pluginId
					+ "'; keeping '" + found->second + "'");
		}

		void BuildProvideIndex(const std::vector<PluginPackage>& packages, ProvideIndex* index,
			std::vector<std::string>* warnings)
		{
			// 项目插件优先(同 id/同扩展名冲突时项目覆盖引擎),两轮之间保持发现顺序 = 确定性。
			for (const PluginScope scope : { PluginScope::Project, PluginScope::Engine })
			{
				for (const PluginPackage& package : packages)
				{
					if (package.Manifest.Scope != scope)
						continue;
					const std::string pluginId = package.Manifest.Id;
					for (const PluginContribution& contribution : package.Manifest.Contributions)
					{
						const char* face = PluginContributionFaceName(contribution.Face);
						switch (contribution.Face)
						{
							case PluginContributionFace::Component:
								InsertProvider(index->Components, contribution.Id, pluginId, face, warnings);
								break;
							case PluginContributionFace::AssetType:
								InsertProvider(index->AssetTypes, contribution.Id, pluginId, face, warnings);
								for (const std::string& extension : contribution.Extensions)
									InsertProvider(index->AssetTypeExtensions, ToLower(extension), pluginId,
										"asset.type extension", warnings);
								break;
							case PluginContributionFace::Importer:
								InsertProvider(index->Importers, contribution.Id, pluginId, face, warnings);
								for (const std::string& extension : contribution.Extensions)
									InsertProvider(index->ImporterExtensions, ToLower(extension), pluginId,
										"asset.importer extension", warnings);
								break;
							case PluginContributionFace::ScriptNamespace:
								InsertProvider(index->ScriptNamespaces, contribution.Id, pluginId, face, warnings);
								break;
						}
					}
				}
			}
		}

		// 内容根下的常规文件遍历(判定失败/权限不足时静默跳过 —— 打包失败由 cook 的其它门负责)。
		std::vector<fs::path> ListContentFiles(const fs::path& contentRoot)
		{
			std::vector<fs::path> files;
			std::error_code ec;
			if (!fs::is_directory(contentRoot, ec))
				return files;
			for (fs::recursive_directory_iterator iterator(contentRoot,
					fs::directory_options::skip_permission_denied, ec);
				!ec && iterator != fs::recursive_directory_iterator(); iterator.increment(ec))
			{
				std::error_code fileEc;
				if (iterator->is_regular_file(fileEc))
					files.push_back(iterator->path());
			}
			std::sort(files.begin(), files.end());
			return files;
		}

		std::string RelativePath(const fs::path& path, const fs::path& root)
		{
			std::error_code ec;
			const fs::path relative = fs::relative(path, root, ec);
			if (ec || relative.empty())
				return path.filename().generic_string();
			return relative.generic_string();
		}

		// ① 场景:**`.wd` 里实体的组件键**(SceneSerializer 写的就是 schema 的类型全名)。
		void ScanSceneComponentReferences(const fs::path& contentRoot, const ProvideIndex& index,
			std::vector<ReferenceHit>* hits)
		{
			if (index.Components.empty())
				return;
			for (const fs::path& file : ListContentFiles(contentRoot))
			{
				if (ToLower(file.extension().string()) != ".wd")
					continue;
				YAML::Node root;
				try
				{
					root = YAML::LoadFile(file.string());
				}
				catch (const std::exception&)
				{
					continue;   // 坏场景文件由场景/资产门负责,引用扫描不重复报
				}
				const YAML::Node entities = root["Entities"];
				if (!entities || !entities.IsSequence())
					continue;
				const std::string relative = RelativePath(file, contentRoot);
				for (size_t entityIndex = 0; entityIndex < entities.size(); ++entityIndex)
				{
					const YAML::Node entity = entities[entityIndex];
					if (!entity.IsMap())
						continue;
					for (const auto& item : entity)
					{
						std::string key;
						try
						{
							key = item.first.as<std::string>("");
						}
						catch (const std::exception&)
						{
							continue;
						}
						const auto provider = index.Components.find(key);
						if (provider == index.Components.end())
							continue;
						hits->push_back({ PluginContributionFace::Component, key,
							relative + " (entity " + std::to_string(entityIndex) + ")", provider->second });
					}
				}
			}
		}

		// ②/③ 资产类型与导入器:内容里的**文件扩展名**就是引用(声明里带 extensions 才可扫)。
		void ScanExtensionReferences(const fs::path& contentRoot,
			const std::map<std::string, std::string>& extensions, PluginContributionFace face,
			std::vector<ReferenceHit>* hits)
		{
			if (extensions.empty())
				return;
			for (const fs::path& file : ListContentFiles(contentRoot))
			{
				const std::string extension = ToLower(file.extension().string());
				if (extension.empty())
					continue;
				const auto provider = extensions.find(extension);
				if (provider == extensions.end())
					continue;
				hits->push_back({ face, extension, RelativePath(file, contentRoot), provider->second });
			}
		}

		bool IsIdentifierChar(char character)
		{
			return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
		}

		// ④ 脚本:`.luau` / `.lua` 里出现的 `<命名空间>.` 调用(`hello.ping(...)`)。
		// 粗粒度静态扫描(注释行跳过;字符串字面量里的同名文本仍可能命中 —— 宁可多报一条
		// 可核对的引用,也不静默放过)。
		void ScanScriptReferences(const fs::path& contentRoot, const ProvideIndex& index,
			std::vector<ReferenceHit>* hits)
		{
			if (index.ScriptNamespaces.empty())
				return;
			for (const fs::path& file : ListContentFiles(contentRoot))
			{
				const std::string extension = ToLower(file.extension().string());
				if (extension != ".luau" && extension != ".lua")
					continue;
				std::ifstream stream(file, std::ios::binary);
				if (!stream)
					continue;
				std::string line;
				size_t lineNumber = 0;
				std::set<std::string> matched;
				while (std::getline(stream, line))
				{
					++lineNumber;
					if (Trim(line).rfind("--", 0) == 0)
						continue;   // 整行注释
					for (const auto& entry : index.ScriptNamespaces)
					{
						const std::string& name = entry.first;
						if (matched.count(name) > 0)
							continue;
						size_t at = line.find(name);
						while (at != std::string::npos)
						{
							const bool startsClean = at == 0 || !IsIdentifierChar(line[at - 1]);
							const size_t dot = at + name.size();
							const bool callShape = dot < line.size() && line[dot] == '.'
								&& dot + 1 < line.size() && IsIdentifierChar(line[dot + 1]);
							if (startsClean && callShape)
								break;
							at = line.find(name, at + 1);
						}
						if (at != std::string::npos)
						{
							matched.insert(name);
							hits->push_back({ PluginContributionFace::ScriptNamespace, name,
								RelativePath(file, contentRoot) + " (line " + std::to_string(lineNumber) + ")",
								entry.second });
						}
					}
				}
			}
		}
	}

	bool DiscoverPluginPackages(const std::filesystem::path& enginePluginsRoot,
		const std::filesystem::path& projectPluginsRoot,
		const std::vector<std::filesystem::path>& devBinaryRoots,
		std::vector<PluginPackage>* out, std::vector<std::string>* diagnostics)
	{
		if (!out)
			return false;
		out->clear();

		const auto scanRoot = [&](const fs::path& root, PluginScope scope)
		{
			std::error_code ec;
			if (!fs::is_directory(root, ec))
				return;   // 缺根 = 0 个插件(与 PluginManager::Discover 同口径)
			std::vector<fs::path> directories;
			for (const fs::directory_entry& item : fs::directory_iterator(root, ec))
			{
				std::error_code entryEc;
				if (item.is_directory(entryEc) && !entryEc)
					directories.push_back(item.path());
			}
			std::sort(directories.begin(), directories.end());

			for (const fs::path& directory : directories)
			{
				const fs::path manifestPath = directory / "plugin.we.yaml";
				std::error_code fileEc;
				if (!fs::is_regular_file(manifestPath, fileEc))
					continue;

				PluginPackage package;
				std::string reason;
				if (!PluginManifest::Load(manifestPath, scope, &package.Manifest, &reason))
				{
					if (diagnostics)
						diagnostics->push_back("plugin package rejected: " + reason);
					continue;
				}
				const std::string id = package.Manifest.Id;

				// 同根重复 id = 拒绝后到者;跨根同 id = 项目插件覆盖引擎插件(方案 §1 优先级)。
				bool duplicateSameScope = false;
				for (size_t index = 0; index < out->size(); ++index)
				{
					if ((*out)[index].Manifest.Id != id)
						continue;
					if ((*out)[index].Manifest.Scope == scope)
					{
						duplicateSameScope = true;
						break;
					}
					if (scope == PluginScope::Project)
					{
						if (diagnostics)
							diagnostics->push_back("project plugin '" + id + "' overrides the engine plugin '"
								+ id + "' (" + (*out)[index].Manifest.ManifestPath.string() + ")");
						out->erase(out->begin() + static_cast<std::ptrdiff_t>(index));
						break;
					}
				}
				if (duplicateSameScope)
				{
					if (diagnostics)
						diagnostics->push_back("duplicate plugin id '" + id + "' in the "
							+ PluginScopeName(scope) + " plugins root; ignoring " + manifestPath.string());
					continue;
				}

				// 产物定位(与 PluginManager::Discover 同一口径):自带 bin/ 优先,其次开发构建根。
				const fs::path packaged = package.Manifest.LibraryPath;
				if (fs::is_regular_file(packaged, ec))
				{
					package.LibraryPath = packaged;
					package.LibraryFound = true;
				}
				else
				{
					package.LibraryPath = packaged;   // 保留"期望路径"供诊断使用
					const std::string name = directory.filename().string();
					for (const fs::path& devRoot : devBinaryRoots)
					{
						const fs::path candidate = devRoot / (name + kPluginLibraryExtension);
						std::error_code candidateEc;
						if (fs::is_regular_file(candidate, candidateEc))
						{
							package.LibraryPath = candidate;
							package.LibraryFound = true;
							break;
						}
					}
				}
				out->push_back(std::move(package));
			}
		};

		scanRoot(enginePluginsRoot, PluginScope::Engine);
		scanRoot(projectPluginsRoot, PluginScope::Project);
		return true;
	}

	PluginPackResult PackagePlugins(const PluginPackRequest& request)
	{
		PluginPackResult result;

		std::vector<PluginPackage> packages;
		DiscoverPluginPackages(request.EnginePluginsRoot, request.ProjectPluginsRoot,
			request.DevBinaryRoots, &packages, &result.Warnings);

		std::map<std::string, size_t> byId;
		for (size_t index = 0; index < packages.size(); ++index)
			byId.emplace(packages[index].Manifest.Id, index);

		std::set<std::string> tolerated(request.TolerateMissing.begin(), request.TolerateMissing.end());
		std::string firstError;

		const auto noteMissing = [&](const std::string& referrer, const std::string& face,
			const std::string& id, const std::string& pluginId, const std::string& advice)
		{
			const std::string diagnostic = referrer + " → " + face + " '" + id + "' → " + advice;
			result.Diagnostics.push_back(diagnostic);
			++result.MissingReferences;
			if (tolerated.count(pluginId) > 0)
				WLD_CORE_ERROR("[plugins] tolerated missing reference: {0}", diagnostic);
			else
			{
				WLD_CORE_ERROR("[plugins] missing reference: {0}", diagnostic);
				if (firstError.empty())
					firstError = diagnostic;
			}
		};

		// ---- 1. 随包闭包 = 显式启用 ∪ 项目插件(默认随包)∪ depends 传递闭包(拓扑序) ----
		std::set<std::string> inClosure;
		std::set<std::string> visiting;
		std::function<bool(const std::string&)> visit = [&](const std::string& id) -> bool
		{
			if (inClosure.count(id) > 0)
				return true;
			const auto found = byId.find(id);
			if (found == byId.end())
				return false;   // 调用方负责诊断(缺插件/缺依赖)
			if (visiting.count(id) > 0)
			{
				if (firstError.empty())
					firstError = "plugin dependency cycle detected at '" + id + "'";
				return false;
			}
			visiting.insert(id);
			for (const std::string& dependency : packages[found->second].Manifest.Depends)
			{
				const auto provider = byId.find(dependency);
				if (provider == byId.end())
				{
					noteMissing("plugin '" + id + "' (depends)", "plugin", dependency, dependency,
						"install/enable the plugin '" + dependency + "' that this dependency names");
					continue;
				}
				if (packages[provider->second].Manifest.Ship == PluginShipPolicy::Never)
				{
					noteMissing("plugin '" + id + "' (depends)", "plugin", dependency, dependency,
						"plugin '" + dependency + "' is marked 'ship: never' but is required as a dependency");
					continue;
				}
				if (!visit(dependency))
					return false;
			}
			visiting.erase(id);
			inClosure.insert(id);
			result.Shipped.push_back(id);
			return true;
		};

		// 显式启用:引擎插件必须在这里出现才随包;项目插件默认随包(ship: never 排除)。
		for (const std::string& id : request.Enabled)
			if (byId.find(id) == byId.end())
			{
				noteMissing("project.we.yaml plugins.enabled", "plugin", id, id,
					"no plugin package with this id is present under the engine/project plugins roots");
			}
		for (const PluginPackage& package : packages)
		{
			const std::string& id = package.Manifest.Id;
			const bool ships = package.Manifest.Scope == PluginScope::Project
				? package.Manifest.Ship != PluginShipPolicy::Never
				: Contains(request.Enabled, id);
			if (!ships)
				continue;
			visit(id);
		}
		// "未随包"= 发现但不在闭包里的插件(依赖闭包会把未显式启用的引擎插件**提进**闭包,
		// 因此这里按最终闭包过滤,而不是按发现期候选)。
		for (const PluginPackage& package : packages)
			if (inClosure.count(package.Manifest.Id) == 0)
				result.Skipped.push_back(package.Manifest.Id);

		// ---- 2. 引用索引 + 内容扫描(闭包外/被禁用/缺件 ⇒ 失败或降级) ----
		ProvideIndex index;
		BuildProvideIndex(packages, &index, &result.Warnings);

		std::vector<ReferenceHit> hits;
		ScanSceneComponentReferences(request.ContentRoot, index, &hits);
		ScanExtensionReferences(request.ContentRoot, index.AssetTypeExtensions,
			PluginContributionFace::AssetType, &hits);
		ScanExtensionReferences(request.ContentRoot, index.ImporterExtensions,
			PluginContributionFace::Importer, &hits);
		ScanScriptReferences(request.ContentRoot, index, &hits);

		for (const ReferenceHit& hit : hits)
		{
			switch (hit.Face)
			{
				case PluginContributionFace::Component: ++result.ComponentReferences; break;
				case PluginContributionFace::AssetType: ++result.AssetTypeReferences; break;
				case PluginContributionFace::Importer: ++result.ImporterReferences; break;
				case PluginContributionFace::ScriptNamespace: ++result.ScriptReferences; break;
			}
			if (inClosure.count(hit.PluginId) > 0)
				continue;   // 引用落在闭包内 = 随包,不是缺件
			noteMissing(hit.Referrer, PluginContributionFaceName(hit.Face), hit.Id, hit.PluginId,
				"enable plugin '" + hit.PluginId + "' (project.we.yaml plugins.enabled) or declare it "
				"under plugins.tolerate_missing");
		}

		// ---- 3. 拷贝产物(闭包内每个插件的 DLL → <publish>/bin/plugins/) ----
		if (firstError.empty() && request.CopyLibraries && !request.PublishDir.empty())
		{
			const fs::path destinationDir = request.PublishDir / "bin" / "plugins";
			std::set<std::string> fileNames;
			for (const std::string& id : result.Shipped)
			{
				const PluginPackage& package = packages[byId.at(id)];
				if (!package.LibraryFound)
				{
					firstError = "shipped plugin '" + id + "' has no library (looked for "
						+ package.Manifest.LibraryPath.string() + " and the development build roots)";
					break;
				}
				const std::string fileName = package.LibraryPath.filename().string();
				if (!fileNames.insert(ToLower(fileName)).second)
				{
					firstError = "two shipped plugins map to the same library file name '" + fileName + "'";
					break;
				}
				std::error_code directoryEc;
				fs::create_directories(destinationDir, directoryEc);
				if (directoryEc)
				{
					firstError = "cannot create " + destinationDir.string() + ": " + directoryEc.message();
					break;
				}
				const fs::path target = destinationDir / fileName;
				std::error_code sameEc;
				if (fs::is_regular_file(target, sameEc)
					&& fs::equivalent(package.LibraryPath, target, sameEc) && !sameEc)
				{
					++result.CopiedLibraries;   // 源即目标(重复 cook 到同一目录):已经在位
					continue;
				}
				std::error_code copyEc;
				fs::copy_file(package.LibraryPath, target, fs::copy_options::overwrite_existing, copyEc);
				if (copyEc)
				{
					firstError = "cannot copy plugin library " + package.LibraryPath.string() + ": "
						+ copyEc.message();
					break;
				}
				++result.CopiedLibraries;
			}
		}

		// ---- 4. 诊断与摘要 ----
		for (const std::string& warning : result.Warnings)
			WLD_CORE_WARN("[plugins] {0}", warning);
		result.Ok = firstError.empty();
		if (!result.Ok)
			result.Error = firstError;
		result.SummaryLine = "[plugins] shipped=" + JoinIds(result.Shipped)
			+ " skipped=" + JoinIds(result.Skipped)
			+ " missing-references=" + std::to_string(result.MissingReferences);
		WLD_CORE_INFO("[plugins] references: components={0} asset-types={1} importers={2} scripts={3} "
			"render-hooks={4} (face not provided by the plugin ABI; skipped)",
			result.ComponentReferences, result.AssetTypeReferences, result.ImporterReferences,
			result.ScriptReferences, result.RenderHookReferences);
		if (result.CopiedLibraries > 0)
			WLD_CORE_INFO("[plugins] copied {0} plugin libraries into {1}",
				result.CopiedLibraries, (request.PublishDir / "bin" / "plugins").string());
		if (result.Ok)
			WLD_CORE_INFO("{0}", result.SummaryLine);
		else
			WLD_CORE_ERROR("{0}", result.SummaryLine);
		return result;
	}
}
