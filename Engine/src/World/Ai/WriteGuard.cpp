#include "wldpch.h"
#include "World/Ai/WriteGuard.h"
#include "World/Ai/FingerprintValidator.h"
#include "World/Utils/Paths.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace World::Ai
{
	namespace fs = std::filesystem;

	WriteGuard& WriteGuard::Get()
	{
		static WriteGuard instance;
		return instance;
	}

	bool WriteGuard::MatchGlob(const std::string& pattern, const std::string& text)
	{
		// 支持精确匹配、前缀目录通配 "dir/**"、后缀扩展名通配 "*.ext"
		if (pattern == "**" || pattern == "*")
			return true;

		if (pattern.size() >= 3 && pattern.substr(pattern.size() - 3) == "/**")
		{
			const std::string prefix = pattern.substr(0, pattern.size() - 3);
			if (text == prefix || text.rfind(prefix + "/", 0) == 0 || text.rfind(prefix + "\\", 0) == 0)
				return true;
		}

		if (pattern.rfind("*.", 0) == 0)
		{
			const std::string ext = pattern.substr(1);
			if (text.size() >= ext.size() && text.substr(text.size() - ext.size()) == ext)
				return true;
		}

		return pattern == text;
	}

	bool WriteGuard::IsEngineSourceTree(const fs::path& path) const
	{
		const std::string pathStr = path.lexically_normal().generic_u8string();

		static const char* protectedRoots[] = {
			"Engine/", "Editor/", "Runtime/", "third_party/", "vendor/", "plugins/"
		};

		for (const char* root : protectedRoots)
		{
			if (pathStr.rfind(root, 0) == 0 || pathStr.find("/" + std::string(root)) != std::string::npos)
				return true;
		}

		return false;
	}

	bool WriteGuard::CanWrite(const std::string& targetPath,
		const std::string& projectRoot,
		const WritePermission* perm,
		std::string& outReason) const
	{
		if (targetPath.empty())
		{
			outReason = "Target write path is empty.";
			return false;
		}

		const fs::path root = fs::u8path(projectRoot.empty() ? Paths::ProjectDir().string() : projectRoot).lexically_normal();
		fs::path absTarget = fs::u8path(targetPath);
		if (absTarget.is_relative())
			absTarget = (root / absTarget).lexically_normal();
		else
			absTarget = absTarget.lexically_normal();

		// 1. 绝对红线：引擎源树物理拒写
		if (IsEngineSourceTree(absTarget))
		{
			outReason = "Forbidden: Cannot write into protected engine source tree (" + absTarget.string() + ").";
			return false;
		}

		// 2. 沙箱隔离：禁止路径穿越出项目根
		std::string rootStr = root.generic_u8string();
		std::string targetStr = absTarget.generic_u8string();
		if (!rootStr.empty() && rootStr.back() != '/')
			rootStr += '/';

		if (targetStr.rfind(rootStr, 0) != 0 && targetStr != root.generic_u8string())
		{
			outReason = "Forbidden: Write path escapes project root sandbox.";
			return false;
		}

		// 计算项目相对路径
		std::string relPath;
		if (targetStr.size() >= rootStr.size())
			relPath = targetStr.substr(rootStr.size());
		else
			relPath = targetStr;

		// 3. 角色与阶段白名单权限判定
		if (perm)
		{
			for (const auto& denied : perm->deniedGlobs)
			{
				if (MatchGlob(denied, relPath))
				{
					outReason = "Forbidden: Write path matches denied glob: " + denied;
					return false;
				}
			}

			if (!perm->allowedGlobs.empty())
			{
				bool allowed = false;
				for (const auto& pat : perm->allowedGlobs)
				{
					if (MatchGlob(pat, relPath))
					{
						allowed = true;
						break;
					}
				}
				if (!allowed)
				{
					outReason = "Forbidden: Write path not in role's allowed globs.";
					return false;
				}
			}
		}

		return true;
	}

	bool WriteGuard::RecordWrite(const std::string& targetPath,
		const std::string& projectRoot,
		const std::string& roleId,
		const std::string& sessionId)
	{
		const fs::path root = fs::u8path(projectRoot.empty() ? Paths::ProjectDir().string() : projectRoot);
		const fs::path logDir = root / ".we" / "ai" / "writes";

		std::error_code ec;
		fs::create_directories(logDir, ec);

		const std::string sid = sessionId.empty() ? "default" : sessionId;
		const fs::path logFile = logDir / (sid + ".jsonl");

		std::ofstream out(logFile, std::ios::app);
		if (!out)
			return false;

		const fs::path absPath = fs::u8path(targetPath).is_absolute() ? fs::u8path(targetPath) : (root / targetPath);
		const std::string hash = fs::exists(absPath) ? FingerprintValidator::ComputeFileSha256(absPath) : "";

		const auto now = std::chrono::system_clock::now().time_since_epoch().count();

		out << "{\"time\":" << now
		    << ",\"role\":\"" << roleId << "\""
		    << ",\"file\":\"" << fs::relative(absPath, root, ec).generic_u8string() << "\""
		    << ",\"sha256\":\"" << hash << "\"}\n";

		return true;
	}
}
