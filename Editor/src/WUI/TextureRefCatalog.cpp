#include "wldpch.h"
#include "TextureRefCatalog.h"

// 资产读盘 / 源图解析 / 产物状态:与纹理设置面板、烘焙器**同一份**编辑器侧口径
// (`LoadTextureAssetDocument` / `ResolveTextureSource` / `InspectTextureArtifact` 的唯一实现在那边)。
#include "Panels/TextureSettingsPanel.h"

#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/TextureImportSettings.h"
#include "World/WUI/WuiLocalization.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <map>

namespace World
{
	namespace Editor
	{
		namespace
		{
			// 候选清单的 TTL(每帧要列表,不每帧扫盘;与面板既有 1.5s 口径一致)。
			constexpr double kCatalogTtlSeconds = 1.5;

			double WallClockSeconds()
			{
				return std::chrono::duration<double>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
			}

			// 逻辑路径的小写扩展名(含点)。
			std::string LowerExtension(const std::string& path)
			{
				const size_t dot = path.find_last_of('.');
				if (dot == std::string::npos)
					return {};
				std::string extension = path.substr(dot);
				std::transform(extension.begin(), extension.end(), extension.begin(),
					[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
				return extension;
			}

			bool IsTextureSourceExtension(const std::string& extension)
			{
				return extension == ".png" || extension == ".jpg" || extension == ".jpeg"
					|| extension == ".tga" || extension == ".bmp";
			}

			void AppendBadge(std::string& tokens, const char* token)
			{
				if (!tokens.empty())
					tokens += ',';
				tokens += token;
			}

			// ---- 徽标 token 表(P10 词表)----
			//
			// 顺序 = 画出来的优先序:种类 → 完整性 → 资产形态 → 产物状态。组件推导状态 token 时
			// 自己按严重度查表(`WuiTexturePickerDeriveState`),与本顺序无关。
			void AddBadges(TextureRefFacts& facts, const std::string& normalized)
			{
				if (!facts.Known)
					return;
				AppendBadge(facts.Badges, facts.IsAsset ? "asset" : "source");
				if (facts.IsAsset)
				{
					if (!facts.AssetExists)
						AppendBadge(facts.Badges, "missing");
					else if (!facts.Readable)
						AppendBadge(facts.Badges, "unreadable");
					else
					{
						if (facts.Container)
							AppendBadge(facts.Badges, "container");
						else if (facts.Legacy)
							AppendBadge(facts.Badges, "legacy");
						if (!facts.SourceExists)
							AppendBadge(facts.Badges, "missing-source");
					}
				}
				else if (!facts.SourceExists)
					AppendBadge(facts.Badges, "missing");

				// 产物状态(源图 / 资产都算,与纹理设置面板同一份判定):
				// 与"这份源 + 这份设置"一致 = baked;源或设置改过 = stale(需重烘);没有产物 = unbaked。
				const bool judgeArtifact = facts.IsAsset
					? (facts.AssetExists && facts.Readable) : facts.SourceExists;
				if (!judgeArtifact)
					return;
				switch (InspectTextureArtifact(ContentRootPath(), normalized, nullptr).State)
				{
					case TextureArtifactState::Fresh: AppendBadge(facts.Badges, "baked"); break;
					case TextureArtifactState::Stale: AppendBadge(facts.Badges, "stale"); break;
					case TextureArtifactState::NoArtifact: AppendBadge(facts.Badges, "unbaked"); break;
					case TextureArtifactState::Invalid: AppendBadge(facts.Badges, "unreadable"); break;
					case TextureArtifactState::NoSource: break;   // 上面已有 missing / missing-source
				}
			}

			// ---- 候选清单缓存(TTL;UI 线程独占,不做锁)----
			struct EntryCache
			{
				std::vector<TextureRefEntry> Entries;
				double Stamp = -1.0;
			};

			EntryCache& Cache()
			{
				static EntryCache cache;
				return cache;
			}

			// ---- 当前值的徽标 / 状态记忆(同 TTL;UI 线程独占)----
			//
			// 为什么必须记:徽标里的"已烘焙 / 需重烘"要读产物头 + 算源图哈希
			// (`InspectTextureArtifact`)。组件每帧都要徽标串,不记就是"每帧对同一张纹理重算一遍
			// 哈希"(4K 纹理 ≈ 16MB/帧)。记忆按逻辑路径,TTL 与候选清单同一条;
			// 导入/赋值后由 `InvalidateTextureRefCatalog()` 一起失效。
			struct DecorationCacheEntry
			{
				double Stamp = -1.0;
				std::string Badges;
			};

			std::map<std::string, DecorationCacheEntry>& DecorationCache()
			{
				static std::map<std::string, DecorationCacheEntry> cache;
				return cache;
			}

			bool HasAssetWithSameStem(const std::vector<TextureRefEntry>& entries, const std::string& logical)
			{
				const std::filesystem::path source(logical);
				for (const TextureRefEntry& entry : entries)
				{
					if (!IsTextureAssetPath(entry.Value))
						continue;
					const std::filesystem::path asset(entry.Value);
					if (asset.parent_path().generic_string() == source.parent_path().generic_string()
						&& asset.stem().string() == source.stem().string())
						return true;
				}
				return false;
			}

			// 一轮纹理引用的**唯一扫描**(内容根,按逻辑路径排序):
			//   ① `.wtex` 资产 —— Label = 资产逻辑路径(括号里带导入源名,按熟名字也能搜);
			//   ② 没有同目录同主名资产的源图 —— Label = 源图逻辑路径;
			//   ③ 同目录同主名只留资产条目(一份设置资产 = 一个用户可见的纹理)。
			// 解析失败(资产坏 / 源图缺)仍进表并带徽标:条目要在下拉里看得见,问题由校验区/行内提示说。
			std::vector<TextureRefEntry> BuildEntries()
			{
				std::vector<TextureRefEntry> entries;
				std::error_code ec;
				const std::filesystem::path root = ContentRootPath();
				if (!std::filesystem::exists(root, ec))
					return entries;
				std::vector<std::string> assetPaths;
				std::vector<std::string> sourcePaths;
				for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(root,
					std::filesystem::directory_options::skip_permission_denied, ec))
				{
					if (!entry.is_regular_file(ec))
						continue;
					const std::string extension = LowerExtension(entry.path().generic_string());
					const bool asset = extension == ".wtex";
					if (!asset && !IsTextureSourceExtension(extension))
						continue;
					const std::filesystem::path relative = std::filesystem::relative(entry.path(), root, ec);
					if (ec)
						continue;
					const std::string logical = MaterialLibrary::NormalizePath(relative.generic_string());
					(asset ? assetPaths : sourcePaths).push_back(logical);
				}
				std::sort(assetPaths.begin(), assetPaths.end());
				std::sort(sourcePaths.begin(), sourcePaths.end());

				for (const std::string& logical : assetPaths)
				{
					const TextureRefFacts facts = DescribeTextureRef(logical);
					TextureRefEntry entry;
					entry.Value = logical;
					entry.Label = logical;
					entry.Badges = facts.Badges;
					const std::string sourceName =
						std::filesystem::path(facts.ImportSource).filename().string();
					if (!sourceName.empty()
						&& sourceName != std::filesystem::path(logical).filename().string())
						entry.Label += " (" + sourceName + ")";
					entries.push_back(std::move(entry));
				}

				for (const std::string& logical : sourcePaths)
				{
					if (HasAssetWithSameStem(entries, logical))
						continue;
					const TextureRefFacts facts = DescribeTextureRef(logical);
					TextureRefEntry entry;
					entry.Value = logical;
					entry.Label = logical;
					entry.Badges = facts.Badges;
					entries.push_back(std::move(entry));
				}
				std::sort(entries.begin(), entries.end(),
					[](const TextureRefEntry& left, const TextureRefEntry& right)
					{
						return left.Value < right.Value;
					});
				return entries;
			}
		}

		std::filesystem::path ContentRootPath()
		{
			return std::filesystem::path(std::string(WLD_PROJECT_DIR)) / "assets";
		}

		// 一条引用的**便宜**事实(不含徽标/产物判定:每帧的校验、行内提示、工具提示走这一条)。
		TextureRefFacts TextureRefFactsFor(const std::string& logical)
		{
			TextureRefFacts facts;
			if (logical.empty())
				return facts;
			facts.Known = true;
			const std::string normalized = MaterialLibrary::NormalizePath(logical);
			const std::filesystem::path declared(normalized);
			facts.InContentRoot = !declared.is_absolute() && !declared.has_root_name();
			for (const std::filesystem::path& part : declared)
				if (part == "..")
					facts.InContentRoot = false;
			facts.IsAsset = IsTextureAssetPath(normalized);
			const std::filesystem::path root = ContentRootPath();
			const std::filesystem::path file = declared.is_absolute() ? declared : root / declared;
			std::error_code fileError;
			if (facts.IsAsset)
			{
				facts.AssetExists = std::filesystem::is_regular_file(file, fileError);
				if (!facts.AssetExists)
					return facts;   // 资产不在盘上:没有可读的设置/源图事实
				// M4-TEX P9:`.wtex` 一律按资产文件读(容器 = 设置 + 内嵌源字节;旧式 = 外部源图)。
				const TextureAssetDocument document = LoadTextureAssetDocument(root, normalized);
				facts.Container = document.Container;
				facts.Legacy = document.Exists && !document.Container;
				facts.Readable = document.Valid;
				if (!document.Valid)
				{
					facts.Error = document.Error;
					return facts;
				}
				TextureSourceResolution resolution;
				if (!ResolveTextureSource(root, normalized, document.Settings, resolution))
				{
					facts.Error = resolution.Error;
					facts.Legacy = true;   // 旧式(或资产头坏)才会走到这里:容器永远能解析
					return facts;
				}
				facts.Embedded = resolution.Embedded;
				facts.Source = resolution.BytesLogical;
				facts.ImportSource = resolution.ImportSourceLogical;
				std::error_code sourceErrorCode;
				// 容器:字节在资产里 ⇒ 一定有"源"(内嵌);旧式:外部源图必须在场。
				facts.SourceExists = resolution.Embedded
					|| (!resolution.BytesLogical.empty() && std::filesystem::is_regular_file(
						root / std::filesystem::path(resolution.BytesLogical), sourceErrorCode));
				return facts;
			}
			facts.Source = normalized;
			facts.SourceExists = std::filesystem::is_regular_file(file, fileError);
			return facts;
		}

		TextureRefFacts DescribeTextureRef(const std::string& logical)
		{
			TextureRefFacts facts = TextureRefFactsFor(logical);
			if (!facts.Known)
				return facts;   // 空引用 = 没有事实(State 保持 Empty)
			const std::string normalized = MaterialLibrary::NormalizePath(logical);
			DecorationCacheEntry& memo = DecorationCache()[normalized];
			const double now = WallClockSeconds();
			if (!(memo.Stamp >= 0.0 && now - memo.Stamp < kCatalogTtlSeconds))
			{
				TextureRefFacts decorated = facts;
				AddBadges(decorated, normalized);
				memo.Stamp = now;
				memo.Badges = decorated.Badges;
			}
			facts.Badges = memo.Badges;
			facts.State = Wui::WuiTexturePickerDeriveState(normalized, facts.Badges);
			return facts;
		}

		const std::vector<TextureRefEntry>& TextureRefEntries()
		{
			EntryCache& cache = Cache();
			const double now = WallClockSeconds();
			if (cache.Stamp >= 0.0 && now - cache.Stamp < kCatalogTtlSeconds)
				return cache.Entries;
			cache.Stamp = now;
			cache.Entries = BuildEntries();
			return cache.Entries;
		}

		void InvalidateTextureRefCatalog()
		{
			Cache().Stamp = -1.0;
			DecorationCache().clear();
		}

		Wui::WuiTexturePickerOptions TexturePickerOptions(const std::string& value, const std::string& label,
			const std::string& idPrefix, const std::string& droppedValue, bool readOnly)
		{
			Wui::WuiTexturePickerOptions options;
			options.Label = label;
			options.IdPrefix = idPrefix;
			const std::vector<TextureRefEntry>& entries = TextureRefEntries();
			options.Entries.reserve(entries.size());
			for (const TextureRefEntry& entry : entries)
				options.Entries.push_back(Wui::WuiTexturePickerEntry { entry.Value, entry.Label, entry.Badges });
			const TextureRefFacts facts = DescribeTextureRef(value);
			options.Badges = facts.Badges;
			options.State = facts.State;
			// 当前值不在候选清单里时的兜底条目:括号里带这半截(盘上的导入源名)。
			options.ImportSourceName = std::filesystem::path(facts.ImportSource).filename().string();
			// 面板侧文案:清空项沿用面板既有 key(用户可见措辞与换用前一致)。
			options.NoneLabel = Wui::Tr("panel.material.texture_none", "(none)");
			options.DroppedValue = droppedValue;
			options.ReadOnly = readOnly;
			return options;
		}

		// M4-TEX P9:赋纹理前先"确保资产" —— 选中的若是**源图**而它还没有同主名 `.wtex`,
		// 当场导入成**单文件容器**(设置 + 该图片字节),并让材质引用资产(一张纹理 = 一个文件)。
		// 已存在资产 = 原样返回资产路径(不动 payload);导入失败不拦引用(与既有校验口径一致)。
		std::string NormalizeTextureChoice(const std::string& logical, std::string* outNote)
		{
			if (outNote)
				outNote->clear();
			if (logical.empty())
				return logical;
			const std::string normalized = MaterialLibrary::NormalizePath(logical);
			if (IsTextureAssetPath(normalized))
				return normalized;
			if (!IsTextureSourceExtension(LowerExtension(normalized)))
				return normalized;   // 非纹理扩展名交给调用方既有的可读反馈
			std::string assetLogical;
			bool created = false;
			std::string error;
			if (!EnsureTextureAssetForSource(ContentRootPath(), normalized, &assetLogical, &created, error))
			{
				if (outNote)
					*outNote = Wui::TrFormat("panel.material.status.texture_import_failed",
						"Could not import this image as a texture asset: {detail}",
						{ { "detail", error } });
				WLD_CORE_WARN("[material-ui] texture import failed for '{0}': {1}", normalized, error);
				return normalized;
			}
			if (outNote && created)
				*outNote = Wui::TrFormat("panel.material.status.texture_imported",
					"Imported {source} as a single-file texture asset: {asset}",
					{ { "source", normalized }, { "asset", assetLogical } });
			if (created)
				WLD_CORE_INFO("[material-ui] assigned source '{0}' -> imported container '{1}'", normalized,
					assetLogical);
			return assetLogical;
		}

		// "资产 → 源图"关系 + 缺失原因(工具提示与无障碍节点共用一句;空引用返回空串)。
		std::string TextureRefDoc(const std::string& logical)
		{
			if (logical.empty())
				return {};
			const TextureRefFacts info = TextureRefFactsFor(logical);
			std::string doc = Wui::Tr("panel.material.texture.ref.tooltip", "Referenced texture: ") + logical;
			if (info.IsAsset)
			{
				if (info.Embedded)
				{
					// M4-TEX P9:单文件容器 —— 设置与源字节都在这个 `.wtex` 里(源图只是导入源)。
					doc += "\n" + Wui::TrFormat("panel.material.texture.ref.asset_embedded",
						"Single-file texture asset: {asset} — import settings and source bytes live in this "
						"one file",
						{ { "asset", logical } });
					if (!info.ImportSource.empty())
						doc += "\n" + Wui::TrFormat("panel.material.texture.ref.asset_import_source",
							"Import source on disk (optional): {source}",
							{ { "source", info.ImportSource } });
				}
				else
				{
					const std::string source = info.Source.empty()
						? Wui::Tr("panel.material.texture.ref.source_none", "(no source image)")
						: info.Source;
					doc += "\n" + Wui::TrFormat("panel.material.texture.ref.asset_source",
						"Texture asset → source image: {asset} → {source}",
						{ { "asset", logical }, { "source", source } });
				}
				if (!info.AssetExists)
					doc += "\n" + Wui::Tr("panel.material.texture.ref.asset_missing",
						"The texture asset file is missing on disk (the .wtex was moved, renamed or deleted).");
				else if (!info.Embedded && !info.Error.empty())
					doc += "\n" + Wui::Tr("panel.material.texture.ref.source_unresolved",
						"The source image cannot be resolved from this asset: ") + info.Error;
				else if (!info.Embedded && !info.SourceExists)
					doc += "\n" + Wui::Tr("panel.material.texture.ref.source_missing",
						"The source image the asset points at is missing on disk.");
				if (info.Legacy && !info.Embedded)
					doc += "\n" + Wui::Tr("panel.material.texture.ref.legacy_hint",
						"Old-style settings file (settings only): re-import the image in the editor to get a "
						"single-file asset.");
			}
			else if (!info.SourceExists)
			{
				doc += "\n" + Wui::Tr("panel.material.texture.ref.image_missing",
					"The image file is missing on disk.");
			}
			if (!info.InContentRoot)
				doc += "\n" + Wui::Tr("panel.material.texture.ref.outside",
					"The path is outside the content root, so a packaged build will not ship it.");
			return doc;
		}

		// 行内校验的一句话(不是静默通过):源图缺失 / 资产缺失 / 资产 → 源图缺失都各有一句。
		std::string TextureInlineWarning(const std::string& logical)
		{
			if (logical.empty())
				return {};
			const TextureRefFacts info = TextureRefFactsFor(logical);
			if (!info.IsAsset)
			{
				if (info.SourceExists)
					return {};
				return Wui::Tr("panel.material.texture.warn.image_missing",
					"Image not found on disk: ") + logical;
			}
			if (!info.AssetExists)
				return Wui::Tr("panel.material.texture.warn.asset_missing",
					"Texture asset not found on disk (moved, renamed or deleted): ") + logical;
			// M4-TEX P9:单文件容器 = 有效(源字节内嵌),外部源图缺失**不是**问题。
			if (info.Embedded)
				return {};
			// 旧式(没有内嵌 payload):源图缺失/解析不了才是问题,并给"可重新导入"的提示。
			if (!info.Error.empty() || !info.SourceExists)
			{
				const std::string source = info.Source.empty()
					? Wui::Tr("panel.material.texture.ref.source_none", "(no source image)")
					: info.Source;
				if (info.Legacy)
					return Wui::Tr("panel.material.texture.warn.legacy_source",
						"Old-style settings file without embedded bytes and no usable source image: ")
						+ logical + " → " + source + " — "
						+ Wui::Tr("panel.material.texture.warn.legacy_hint",
							"re-import the image in the editor to get a single-file asset");
				if (!info.Error.empty())
					return Wui::Tr("panel.material.texture.warn.source_unresolved",
						"The asset's source image cannot be resolved: ") + logical + " — " + info.Error;
				return Wui::Tr("panel.material.texture.warn.source_missing",
					"Texture asset has no source image: ") + logical + " → " + source;
			}
			return {};
		}
	}
}
