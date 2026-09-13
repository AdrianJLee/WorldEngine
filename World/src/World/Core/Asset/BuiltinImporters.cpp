#include "wldpch.h"
#include "World/Core/Asset/BuiltinImporters.h"

#include <fstream>

namespace World::Asset
{
	namespace
	{
		bool ReadSourceFile(const ImportRequest& request, ImportResult& out, std::error_code& ec)
		{
			std::ifstream stream(request.Source, std::ios::binary);
			if (!stream.is_open())
			{
				ec = std::make_error_code(std::errc::no_such_file_or_directory);
				out.Error = "cannot open source: " + request.Source.string();
				return false;
			}
			stream.seekg(0, std::ios::end);
			const std::streamoff size = stream.tellg();
			if (size < 0)
			{
				ec = std::make_error_code(std::errc::io_error);
				out.Error = "cannot size source: " + request.Source.string();
				return false;
			}
			stream.seekg(0, std::ios::beg);
			out.Data.resize(static_cast<size_t>(size));
			if (out.Data.empty())
				return true;
			stream.read(reinterpret_cast<char*>(out.Data.data()), static_cast<std::streamsize>(out.Data.size()));
			if (!stream)
			{
				ec = std::make_error_code(std::errc::io_error);
				out.Data.clear();
				out.Error = "failed to read source: " + request.Source.string();
				return false;
			}
			return true;
		}

		class PassThroughImporter final : public IAssetImporter
		{
		public:
			std::string Name() const override { return "PassThrough"; }
			uint32_t Version() const override { return 1; }
			bool Matches(const std::filesystem::path&) const override { return true; }
			ImportResult Import(const ImportRequest& request, std::error_code& ec) const override
			{
				ImportResult result;
				if (ReadSourceFile(request, result, ec))
					result.Ok = true;
				return result;
			}
		};

		class ExtensionImporter : public IAssetImporter
		{
		public:
			explicit ExtensionImporter(std::string name, uint32_t version, std::string extension)
				: m_Name(std::move(name)), m_Version(version), m_Extension(std::move(extension))
			{
			}

			std::string Name() const override { return m_Name; }
			uint32_t Version() const override { return m_Version; }
			bool Matches(const std::filesystem::path& source) const override
			{
				return source.extension() == m_Extension;
			}
			ImportResult Import(const ImportRequest& request, std::error_code& ec) const override
			{
				// P1:场景与脚本原样复制,加工(规范化/Luau 字节码)在 P2 接入。
				ImportResult result;
				if (ReadSourceFile(request, result, ec))
					result.Ok = true;
				return result;
			}

		private:
			std::string m_Name;
			uint32_t m_Version;
			std::string m_Extension;
		};
	}

	std::vector<std::shared_ptr<IAssetImporter>> DefaultImporters()
	{
		return {
			std::make_shared<ExtensionImporter>("Scene", 1, ".wd"),
			std::make_shared<ExtensionImporter>("Script", 1, ".lua"),
			std::make_shared<PassThroughImporter>(),
		};
	}
}
