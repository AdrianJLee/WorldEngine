#include "CredentialManager.h"

#include <windows.h>
#include <wincrypt.h>

#include <filesystem>
#include <fstream>
#include <vector>

#pragma comment(lib, "crypt32.lib")

namespace World::Gen
{
	namespace fs = std::filesystem;

	namespace
	{
		fs::path GetCredentialsStorageDir()
		{
			// 仅在当前工作区本地受保护的 local/ 目录下存放密文二进制，不进 git
			const fs::path dir = fs::current_path() / "local" / "credentials";
			std::error_code ec;
			fs::create_directories(dir, ec);
			return dir;
		}
	}

	CredentialManager& CredentialManager::Get()
	{
		static CredentialManager instance;
		return instance;
	}

	bool CredentialManager::StoreCredential(const std::string& serviceName, const std::string& plainKey)
	{
		if (serviceName.empty() || plainKey.empty())
			return false;

		DATA_BLOB dataIn;
		dataIn.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plainKey.data()));
		dataIn.cbData = static_cast<DWORD>(plainKey.size());

		DATA_BLOB dataOut;
		if (!CryptProtectData(&dataIn, L"WorldEngine Key", nullptr, nullptr, nullptr, 0, &dataOut))
			return false;

		const fs::path target = GetCredentialsStorageDir() / (serviceName + ".bin");
		std::ofstream out(target, std::ios::binary);
		if (!out)
		{
			LocalFree(dataOut.pbData);
			return false;
		}

		out.write(reinterpret_cast<const char*>(dataOut.pbData), dataOut.cbData);
		out.close();

		LocalFree(dataOut.pbData);
		return true;
	}

	std::string CredentialManager::RetrieveCredential(const std::string& serviceName) const
	{
		const fs::path target = GetCredentialsStorageDir() / (serviceName + ".bin");
		if (!fs::exists(target))
			return "";

		std::ifstream in(target, std::ios::binary);
		if (!in)
			return "";

		std::vector<BYTE> cipherData((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		in.close();

		DATA_BLOB dataIn;
		dataIn.pbData = cipherData.data();
		dataIn.cbData = static_cast<DWORD>(cipherData.size());

		DATA_BLOB dataOut;
		if (!CryptUnprotectData(&dataIn, nullptr, nullptr, nullptr, nullptr, 0, &dataOut))
			return "";

		std::string result(reinterpret_cast<const char*>(dataOut.pbData), dataOut.cbData);
		LocalFree(dataOut.pbData);
		return result;
	}

	bool CredentialManager::HasCredential(const std::string& serviceName) const
	{
		const fs::path target = GetCredentialsStorageDir() / (serviceName + ".bin");
		return fs::exists(target);
	}

	bool CredentialManager::DeleteCredential(const std::string& serviceName)
	{
		const fs::path target = GetCredentialsStorageDir() / (serviceName + ".bin");
		std::error_code ec;
		return fs::remove(target, ec);
	}
}
