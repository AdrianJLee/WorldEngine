#include "wldpch.h"
#include "World/Ai/ProjectPackager.h"
#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"
#include "World/Asset/CookPipeline.h"
#include "World/Asset/BuiltinImporters.h"
#include "World/Asset/ProjectManifest.h"
#include "World/Utils/Paths.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace World::Ai
{
	namespace fs = std::filesystem;
	using namespace World::Wui;

	std::string PackageResult::ToJson() const
	{
		JsonValue root;
		root.type = JsonValue::Type::Object;

		root.Object.push_back({ "success", JsonValue::MakeBool(success) });
		root.Object.push_back({ "profile", JsonValue::MakeString(profile) });
		root.Object.push_back({ "output_directory", JsonValue::MakeString(outputDirectory) });
		root.Object.push_back({ "executable_path", JsonValue::MakeString(executablePath) });
		root.Object.push_back({ "artifacts_count", JsonValue::MakeNumber(static_cast<double>(artifactsCount)) });
		root.Object.push_back({ "total_size_bytes", JsonValue::MakeNumber(static_cast<double>(totalSizeBytes)) });

		JsonValue errArr;
		errArr.type = JsonValue::Type::Array;
		for (const auto& err : errors)
			errArr.Array.push_back(JsonValue::MakeString(err));
		root.Object.push_back({ "errors", std::move(errArr) });

		JsonValue warnArr;
		warnArr.type = JsonValue::Type::Array;
		for (const auto& w : warnings)
			warnArr.Array.push_back(JsonValue::MakeString(w));
		root.Object.push_back({ "warnings", std::move(warnArr) });

		return root.Dump();
	}

	PackageConfig ProjectPackager::ParseConfig(const std::map<std::string, std::string>& args)
	{
		PackageConfig config;
		if (args.count("root"))
			config.projectRoot = args.at("root");

		if (args.count("profile"))
			config.profile = args.at("profile");

		if (args.count("output"))
			config.outputDir = args.at("output");

		return config;
	}

	PackageResult ProjectPackager::Package(const PackageConfig& config)
	{
		PackageResult result;
		result.profile = config.profile.empty() ? "debug" : config.profile;

		fs::path rootPath;
		if (!config.projectRoot.empty())
			rootPath = fs::u8path(config.projectRoot);
		else if (fs::exists(Paths::ProjectDir()))
			rootPath = Paths::ProjectDir();
		else
			rootPath = fs::current_path();

		const fs::path manifestPath = rootPath / "project.we.yaml";
		if (!fs::exists(manifestPath))
		{
			result.errors.push_back("Manifest 'project.we.yaml' not found in: " + rootPath.string());
			result.success = false;
			return result;
		}

		fs::path outDir;
		if (!config.outputDir.empty())
			outDir = fs::u8path(config.outputDir);
		else
			outDir = rootPath / "build" / "package" / result.profile;

		result.outputDirectory = outDir.string();

		std::error_code ec;
		fs::create_directories(outDir / "bin", ec);
		fs::create_directories(outDir / "assets", ec);
		fs::create_directories(outDir / "cooked", ec);

		// 1. 资产烘焙与打包
		Asset::ProjectManifest manifest;
		std::string loadErr;
		if (Asset::ProjectManifest::Load(manifestPath, &manifest, &loadErr))
		{
			Asset::CookPipeline pipeline(Asset::DefaultImporters());
			Asset::CookSummary summary;
			pipeline.Cook(manifest, manifestPath, outDir / "cooked", true, &summary);
		}
		else
		{
			result.warnings.push_back("Failed to load project manifest: " + loadErr);
		}

		// 复制源资产目录(若存在)
		const fs::path srcAssets = rootPath / "assets";
		if (fs::exists(srcAssets))
		{
			for (const auto& entry : fs::recursive_directory_iterator(srcAssets, ec))
			{
				if (!entry.is_regular_file())
					continue;
				const auto rel = fs::relative(entry.path(), srcAssets);
				const auto dest = outDir / "assets" / rel;
				fs::create_directories(dest.parent_path(), ec);
				fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, ec);
			}
		}

		// 2. 复制二进制 Runtime 与动态库
		std::vector<fs::path> runtimeCandidates = {
			fs::current_path() / "build" / "x64-Debug" / "Runtime" / "Debug" / "Runtime.exe",
			fs::current_path() / "build" / "x64-Debug" / "bin" / "Debug" / "Runtime.exe",
			rootPath / "build" / "x64-Debug" / "Runtime" / "Debug" / "Runtime.exe"
		};

		fs::path foundRuntime;
		for (const auto& cand : runtimeCandidates)
		{
			if (fs::exists(cand))
			{
				foundRuntime = cand;
				break;
			}
		}

		if (!foundRuntime.empty())
		{
			const fs::path destExe = outDir / "bin" / "Runtime.exe";
			fs::copy_file(foundRuntime, destExe, fs::copy_options::overwrite_existing, ec);
			result.executablePath = destExe.string();

			// 复制同目录下的 WorldRuntime.dll
			const fs::path dllSrc = foundRuntime.parent_path() / "WorldRuntime.dll";
			if (fs::exists(dllSrc))
			{
				fs::copy_file(dllSrc, outDir / "bin" / "WorldRuntime.dll", fs::copy_options::overwrite_existing, ec);
			}
		}
		else
		{
			result.warnings.push_back("Runtime executable not found in active build tree; package created in data-only mode.");
		}

		// 3. 统计产物数量与字节
		result.artifactsCount = 0;
		result.totalSizeBytes = 0;
		for (const auto& entry : fs::recursive_directory_iterator(outDir, ec))
		{
			if (entry.is_regular_file())
			{
				result.artifactsCount++;
				result.totalSizeBytes += entry.file_size(ec);
			}
		}

		result.success = result.errors.empty();
		return result;
	}

	void ProjectPackager::RegisterCommands(CommandRegistry& reg)
	{
		CommandDescriptor desc;
		desc.name = "build.package";
		desc.description = "Cook assets and package standalone executable distribution bundle.";
		desc.effect = CommandEffect::Exec;
		desc.scope = CommandScope::Both;
		desc.owner = "engine";
		desc.paramsSchema = "{\"type\":\"object\",\"properties\":{\"root\":{\"type\":\"string\"},\"profile\":{\"type\":\"string\",\"enum\":[\"debug\",\"shipping\"]},\"output\":{\"type\":\"string\"}}}";
		desc.resultSchema = "{\"type\":\"object\",\"properties\":{\"success\":{\"type\":\"boolean\"},\"output_directory\":{\"type\":\"string\"},\"artifacts_count\":{\"type\":\"integer\"},\"total_size_bytes\":{\"type\":\"integer\"}}}";
		desc.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
		{
			PackageConfig config = ParseConfig(args);
			result = Package(config).ToJson();
			return true;
		};
		reg.RegisterCommand(std::move(desc));
	}
}
