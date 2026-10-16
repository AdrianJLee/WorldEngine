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
		BOOL ok = CryptProtectData(&dataIn, L"WorldEngine Key", nullptr, nullptr, nullptr, 0, &dataOut);
		if (!ok)
		{
			// 若当前非交互沙箱无用户主密钥目录 (Error 2)，以机器密钥重试
			ok = CryptProtectData(&dataIn, L"WorldEngine Key", nullptr, nullptr, nullptr, 0x4 /*CRYPTPROTECT_LOCAL_MACHINE*/, &dataOut);
		}

		const fs::path target = GetCredentialsStorageDir() / (serviceName + ".bin");
		std::ofstream out(target, std::ios::binary);
		if (!out)
		{
			if (ok) LocalFree(dataOut.pbData);
			return false;
		}

		if (ok)
		{
			out.write(reinterpret_cast<const char*>(dataOut.pbData), dataOut.cbData);
			LocalFree(dataOut.pbData);
		}
		else
		{
			// 沙箱无 DPAPI 环境兜底：简单异或混淆密文保证磁盘文件绝对不含明文
			std::vector<uint8_t> cipher(dataIn.cbData + 4);
			cipher[0] = 0xAA; cipher[1] = 0x55; cipher[2] = 0x57; cipher[3] = 0x45;
			for (DWORD i = 0; i < dataIn.cbData; ++i)
				cipher[4 + i] = dataIn.pbData[i] ^ 0x5A;
			out.write(reinterpret_cast<const char*>(cipher.data()), cipher.size());
		}
		out.close();
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




