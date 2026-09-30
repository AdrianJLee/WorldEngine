#include "wldpch.h"
#include "TextureArtifactBaker.h"

// 资产读取的**唯一口径**(容器 / 旧式自动区分)与渲染设置解析都在面板模块里,
// 本文件只调用、不复制它的解析实现(避免"第二套读法")。
#include "../WUI/Panels/TextureSettingsPanel.h"

#include "World/Renderer/MaterialTextureCache.h"
#include "World/Renderer/TextureCompiler.h"

#include <fstream>
#include <vector>

namespace World
{
	namespace
	{
		// 源图扩展名(与 TextureCompiler::BakeDirectory / 面板的候选顺序一致)。
		const char* const kSourceCandidates[] = { ".png", ".jpg", ".jpeg", ".tga", ".bmp" };

		// 与 TextureCompiler 的写盘口径一致:临时文件 + 原子替换(失败则先删再换一次)。
		bool WriteFileBytesAtomic(const std::filesystem::path& path, const std::vector<uint8_t>& bytes,
			std::string& error)
		{
			std::error_code directoryError;
			if (!path.parent_path().empty())
				std::filesystem::create_directories(path.parent_path(), directoryError);
			const std::filesystem::path temporary = path.string() + ".tmp-write";
			{
				std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
				if (!output.is_open())
				{
					error = "cannot write " + temporary.generic_string();
					return false;
				}
				output.write(reinterpret_cast<const char*>(bytes.data()),
					static_cast<std::streamsize>(bytes.size()));
			}
			std::error_code renameError;
			std::filesystem::rename(temporary, path, renameError);
			if (renameError)
			{
				std::error_code removeError;
				std::filesystem::remove(path, removeError);
				renameError.clear();
				std::filesystem::rename(temporary, path, renameError);
			}
			if (renameError)
			{
				error = "cannot replace " + path.generic_string() + ": " + renameError.message();
				std::error_code cleanupError;
				std::filesystem::remove(temporary, cleanupError);
				return false;
			}
			return true;
		}

		// 产物落点(与内核 `TextureCompiler::BakeDirectory` / `TextureData` 的查找口径一致):
		// 契约名 = `<同目录>/<源图主名>.wtexc`(2026-09-25 起产物名里不再带源图扩展名)。
		std::filesystem::path ArtifactPathForSource(const std::filesystem::path& sourceFile)
		{
			return sourceFile.parent_path() / (sourceFile.stem().string() + ".wtexc");
		}

		// 容错候选:旧命名 `<源图全名>.wtexc`(内核 lookup 的第二候选)。契约名写成功之后
		// 把它删掉 —— 它是可重建的产物,不是用户数据。
		std::filesystem::path LegacyArtifactPathForSource(const std::filesystem::path& sourceFile)
		{
			return std::filesystem::path(sourceFile.string() + ".wtexc");
		}
	}

	namespace Editor
	{
		bool CommitTextureArtifact(const std::filesystem::path& contentRoot, const std::string& sourceLogical,
			const std::filesystem::path& byteSourceFile, const std::vector<uint8_t>& artifactBytes,
			const TextureArtifactHeader& header, std::string& outError)
		{
			outError.clear();
			const std::filesystem::path artifactFile = ArtifactPathForSource(byteSourceFile);
			std::string writeError;
			if (!WriteFileBytesAtomic(artifactFile, artifactBytes, writeError))
			{
				outError = writeError;
				return false;
			}
			// 旧命名(带源图扩展名)的残留副本:同一份源只保留一份产物 —— 契约名写成功之后
			// 把它删掉(它是可重建的产物,不是用户数据)。运行时的 lookup 会在契约名缺失时
			// 才回退到旧名,所以删除不会让运行时"突然找不到"。
			const std::filesystem::path legacyArtifact = LegacyArtifactPathForSource(byteSourceFile);
			std::error_code legacyError;
			if (std::filesystem::is_regular_file(legacyArtifact, legacyError)
				&& std::filesystem::remove(legacyArtifact, legacyError))
				WLD_CORE_INFO("[texture] removed legacy artifact copy: {0}",
					legacyArtifact.filename().generic_string());
			// 材质贴图缓存按**逻辑路径**失效:下一次 Get 重新读盘(命中新产物)。
			MaterialTextureCache::Get().Invalidate(sourceLogical);
			// 同一份纹理可能被"资产引用"与"源图引用"两种写法引用:两条缓冲键都失效。
			const std::string assetLogical = TextureAssetPathForSource(sourceLogical);
			if (assetLogical != sourceLogical)
				MaterialTextureCache::Get().Invalidate(assetLogical);
			else
			{
				const std::filesystem::path asset(sourceLogical);
				for (const char* extension : kSourceCandidates)
				{
					const std::string sibling =
						(asset.parent_path() / (asset.stem().string() + extension)).generic_string();
					std::error_code siblingError;
					if (std::filesystem::is_regular_file(contentRoot / sibling, siblingError))
						MaterialTextureCache::Get().Invalidate(sibling);
				}
			}
			WLD_CORE_INFO("[texture] baked {0} -> {1} ({2}, {3}x{4}, {5} mips)", sourceLogical,
				artifactFile.filename().generic_string(), TextureBlockFormatName(header.Format),
				header.Width, header.Height, header.MipCount);
			return true;
		}

		bool BakeTextureArtifactNow(const std::filesystem::path& contentRoot,
			const std::string& sourceLogical, const TextureImportSettings& settings, std::string& outError)
		{
			outError.clear();
			if (sourceLogical.empty())
			{
				outError = "no source image";
				return false;
			}
			const std::filesystem::path sourceFile = contentRoot / sourceLogical;
			std::vector<uint8_t> artifactBytes;
			TextureArtifactHeader header;
			std::string bakeError;
			bool baked = false;
			// M4-TEX P9:容器(`.wtex` + 内嵌 payload)按**内嵌字节**烘(与 cook 同一口径:
			// 产物头的 sourceSha256 = sha256(payload)),不读外部源图。
			if (IsTextureAssetPath(sourceLogical))
			{
				const TextureAssetDocument document = LoadTextureAssetDocument(contentRoot, sourceLogical);
				if (!document.Valid)
				{
					outError = document.Error;
					return false;
				}
				if (!document.Container)
				{
					outError = "old-style settings file has no embedded source bytes "
						"(re-import it as a single file: double-click its source image in the Content Browser)";
					return false;
				}
				baked = TextureCompiler::BakeBytes(document.Payload, settings, artifactBytes, header,
					bakeError);
			}
			else
				baked = TextureCompiler::BakeFile(sourceFile, settings, artifactBytes, header, bakeError);
			if (!baked)
			{
				outError = bakeError;
				return false;
			}
			return CommitTextureArtifact(contentRoot, sourceLogical, sourceFile, artifactBytes, header,
				outError);
		}
	}
}
