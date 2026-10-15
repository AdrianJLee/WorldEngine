#pragma once

#include <string>

namespace World::Gen
{
	class CredentialManager
	{
	public:
		static CredentialManager& Get();

		// 通过 Windows DPAPI (CryptProtectData) 密文托管 API Key，绝不落盘明文
		bool StoreCredential(const std::string& serviceName, const std::string& plainKey);

		// 解密读取 API Key
		std::string RetrieveCredential(const std::string& serviceName) const;

		// 检查是否存在凭据
		bool HasCredential(const std::string& serviceName) const;

		// 删除凭据
		bool DeleteCredential(const std::string& serviceName);
	};
}
