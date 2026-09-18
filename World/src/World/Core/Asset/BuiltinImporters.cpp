#include "wldpch.h"
#include "World/Core/Asset/BuiltinImporters.h"

#include "World/Core/Asset/GltfImporter.h"
#include "World/Core/Asset/ModelImportSettings.h"
#include "World/Core/Asset/ScriptArtifact.h"

#include <fstream>
#include <string_view>
#include <utility>

namespace World::Asset
{
	namespace
	{
		// 源文件 → 字节。失败时清空 out 并写 error/ec;空文件算成功(out 为空)。
		bool ReadSourceBytes(const ImportRequest& request, std::vector<uint8_t>& out,
			std::string& error, std::error_code& ec)
		{
			std::ifstream stream(request.Source, std::ios::binary);
			if (!stream.is_open())
			{
				ec = std::make_error_code(std::errc::no_such_file_or_directory);
				error = "cannot open source: " + request.Source.string();
				return false;
			}
			stream.seekg(0, std::ios::end);
			const std::streamoff size = stream.tellg();
			if (size < 0)
			{
				ec = std::make_error_code(std::errc::io_error);
				error = "cannot size source: " + request.Source.string();
				return false;
			}
			stream.seekg(0, std::ios::beg);
			out.resize(static_cast<size_t>(size));
			if (out.empty())
				return true;
			stream.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
			if (!stream)
			{
				ec = std::make_error_code(std::errc::io_error);
				out.clear();
				error = "failed to read source: " + request.Source.string();
				return false;
			}
			return true;
		}

		// 原样复制(W7-2 前 Script 也走这里;现在只剩 PassThrough 与 Scene)。
		ImportResult CopySource(const ImportRequest& request, std::error_code& ec)
		{
			ImportResult result;
			if (ReadSourceBytes(request, result.Data, result.Error, ec))
				result.Ok = true;
			return result;
		}

		class PassThroughImporter final : public IAssetImporter
		{
		public:
			std::string Name() const override { return "PassThrough"; }
			uint32_t Version() const override { return 1; }
			bool Matches(const std::filesystem::path&) const override { return true; }
			ImportResult Import(const ImportRequest& request, std::error_code& ec) const override
			{
				return CopySource(request, ec);
			}
		};

		// 按一组扩展名匹配(W7-2 起需要 .lua + .luau;Scene 仍是单个 .wd)。
		class ExtensionImporter : public IAssetImporter
		{
		public:
			ExtensionImporter(std::string name, uint32_t version, std::vector<std::string> extensions)
				: m_Name(std::move(name)), m_Version(version), m_Extensions(std::move(extensions))
			{
			}

			std::string Name() const override { return m_Name; }
			uint32_t Version() const override { return m_Version; }
			bool Matches(const std::filesystem::path& source) const override
			{
				const std::filesystem::path extension = source.extension();
				for (const std::string& candidate : m_Extensions)
					if (extension == candidate)
						return true;
				return false;
			}
			ImportResult Import(const ImportRequest& request, std::error_code& ec) const override
			{
				// P1:场景原样复制,规范化在 P2 接入。
				return CopySource(request, ec);
			}

		private:
			std::string m_Name;
			uint32_t m_Version;
			std::vector<std::string> m_Extensions;
		};

		// W7-2:Script 导入器。"源码 → 包内容"的唯一入口是 ScriptArtifact::Pack,
		// 产物 = 28 字节 WSL1 头 + Luau 字节码(不再复制源码),失败一律不写产物。
		// Version 1 → 2 是必需:cook 的复合指纹含导入器名与版本,旧 cook.db 因此整体
		// 重烘,包里不会残留源码副本。逻辑路径不变(场景里的 ScriptFilePath 无需改)。
		class ScriptImporter final : public ExtensionImporter
		{
		public:
			ScriptImporter()
				: ExtensionImporter("Script", 2, { ".lua", ".luau" })
			{
			}

			ImportResult Import(const ImportRequest& request, std::error_code& ec) const override
			{
				ImportResult result;
				std::vector<uint8_t> source;
				if (!ReadSourceBytes(request, source, result.Error, ec))
					return result;

				std::string_view sourceView;
				if (!source.empty())
					sourceView = std::string_view(reinterpret_cast<const char*>(source.data()), source.size());
				std::string packError;
				if (!ScriptArtifact::Pack(request.LogicalPath, sourceView, result.Data, &packError))
				{
					// 语法错误等编译失败:Pack 已清空 result.Data,诊断原样保留。
					result.Error = packError;
					ec = std::make_error_code(std::errc::invalid_argument);
					return result;
				}
				result.Ok = true;
				return result;
			}
		};

		// D5b:模型导入器(.gltf/.glb → .wmodel + .wmat + 贴图,单一源产出多产物)。
		//  - 复用 GltfImporter::ImportAsBytes(与编辑器导入/单测同一条内核);
		//  - .wimport 缺失/坏 JSON → 默认值 + Warnings 记录,不失败;
		//  - .wmodel 字节在返回前已由内核用 WModelIO::Parse 自校验,坏模型在 cook 期报错;
		//  - Outputs 顺序 = 贴图 → 材质 → 模型(.wmodel 最后写,cook 失败时不留半成品模型)。
		class ModelImporter final : public IAssetImporter
		{
		public:
			std::string Name() const override { return "Model"; }
			uint32_t Version() const override { return 1; }
			bool Matches(const std::filesystem::path& source) const override
			{
				const std::filesystem::path extension = source.extension();
				return extension == ".gltf" || extension == ".glb";
			}

			ImportResult Import(const ImportRequest& request, std::error_code& ec) const override
			{
				ImportResult result;

				std::string settingsWarning;
				const ModelImportSettings settings =
					ModelImportSettings::Load(request.Source.string(), &settingsWarning);
				if (!settingsWarning.empty())
					result.Warnings.push_back(settingsWarning);

				GltfImportMetadata metadata;
				metadata.ImporterVersion = Version();
				metadata.SettingsHash = ModelImportSettings::Hash(settings);
				metadata.UpAxis = settings.UpAxis;
				metadata.Scale = settings.Scale;
				// 布局与编辑器导入(GltfImporter::ImportFile)同一条:扁平
				// `models/<源 stem>.wmodel` + `materials/` + `textures/`;
				// 源逻辑路径写进 meta,编辑器据此判断"源/设置已变 → 需要重导"。
				metadata.LogicalModelPath.clear();
				metadata.SourceLogicalPath = request.LogicalPath;

				GltfImportBytesResult bytes;
				std::string importError;
				if (!GltfImporter::ImportAsBytes(request.Source.string(), settings, metadata,
					&bytes, &importError))
				{
					result.Error = importError.empty() ? "glTF import failed" : importError;
					ec = std::make_error_code(std::errc::invalid_argument);
					return result;
				}
				if (bytes.Outputs.empty())
				{
					result.Error = "model import produced no outputs: " + request.LogicalPath;
					ec = std::make_error_code(std::errc::invalid_argument);
					return result;
				}
				for (const std::string& warning : bytes.Summary.Warnings)
					result.Warnings.push_back(warning);
				result.Fingerprint = bytes.Summary.SourceFingerprint;
				result.Outputs.reserve(bytes.Outputs.size());
				for (GltfInMemoryOutput& output : bytes.Outputs)
					result.Outputs.push_back({ std::move(output.LogicalPath), std::move(output.Data) });
				result.Ok = true;
				return result;
			}
		};
	}

	std::vector<std::shared_ptr<IAssetImporter>> DefaultImporters()
	{
		// 注册顺序:特定类型在前,PassThrough 兜底;模型在 Scene/Script 之后、PassThrough 之前。
		return {
			std::make_shared<ExtensionImporter>("Scene", 1, std::vector<std::string>{ ".wd" }),
			std::make_shared<ScriptImporter>(),
			std::make_shared<ModelImporter>(),
			std::make_shared<PassThroughImporter>(),
		};
	}
}
