#include "wldpch.h"
#include "World/Ai/FingerprintValidator.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace World::Ai
{
	namespace fs = std::filesystem;

	namespace
	{
		// 标准 SHA-256 算法实现 (FIPS PUB 180-4)
		inline uint32_t RightRotate(uint32_t value, uint32_t count)
		{
			return (value >> count) | (value << (32 - count));
		}

		void Sha256Transform(uint32_t state[8], const uint8_t data[64])
		{
			static const uint32_t K[64] = {
				0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
				0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
				0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
				0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
				0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
				0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
				0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
				0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
			};

			uint32_t W[64];
			for (int i = 0; i < 16; ++i)
			{
				W[i] = (static_cast<uint32_t>(data[i * 4]) << 24) |
				       (static_cast<uint32_t>(data[i * 4 + 1]) << 16) |
				       (static_cast<uint32_t>(data[i * 4 + 2]) << 8) |
				       (static_cast<uint32_t>(data[i * 4 + 3]));
			}
			for (int i = 16; i < 64; ++i)
			{
				const uint32_t s0 = RightRotate(W[i - 15], 7) ^ RightRotate(W[i - 15], 18) ^ (W[i - 15] >> 3);
				const uint32_t s1 = RightRotate(W[i - 2], 17) ^ RightRotate(W[i - 2], 19) ^ (W[i - 2] >> 10);
				W[i] = W[i - 16] + s0 + W[i - 7] + s1;
			}

			uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
			uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

			for (int i = 0; i < 64; ++i)
			{
				const uint32_t S1 = RightRotate(e, 6) ^ RightRotate(e, 11) ^ RightRotate(e, 25);
				const uint32_t ch = (e & f) ^ ((~e) & g);
				const uint32_t temp1 = h + S1 + ch + K[i] + W[i];
				const uint32_t S0 = RightRotate(a, 2) ^ RightRotate(a, 13) ^ RightRotate(a, 22);
				const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
				const uint32_t temp2 = S0 + maj;

				h = g;
				g = f;
				f = e;
				e = d + temp1;
				d = c;
				c = b;
				b = a;
				a = temp1 + temp2;
			}

			state[0] += a; state[1] += b; state[2] += c; state[3] += d;
			state[4] += e; state[5] += f; state[6] += g; state[7] += h;
		}
	}

	std::string FingerprintValidator::ComputeSha256(const void* data, size_t size)
	{
		uint32_t state[8] = {
			0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
			0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
		};

		const uint8_t* ptr = static_cast<const uint8_t*>(data);
		size_t remaining = size;

		while (remaining >= 64)
		{
			Sha256Transform(state, ptr);
			ptr += 64;
			remaining -= 64;
		}

		uint8_t buffer[64] = { 0 };
		std::memcpy(buffer, ptr, remaining);
		buffer[remaining] = 0x80;

		if (remaining >= 56)
		{
			Sha256Transform(state, buffer);
			std::memset(buffer, 0, 64);
		}

		const uint64_t totalBits = static_cast<uint64_t>(size) * 8;
		for (int i = 0; i < 8; ++i)
		{
			buffer[63 - i] = static_cast<uint8_t>((totalBits >> (i * 8)) & 0xFF);
		}
		Sha256Transform(state, buffer);

		std::ostringstream ss;
		for (int i = 0; i < 8; ++i)
		{
			ss << std::hex << std::setw(8) << std::setfill('0') << state[i];
		}
		return ss.str();
	}

	std::string FingerprintValidator::ComputeStringSha256(const std::string& text)
	{
		return ComputeSha256(text.data(), text.size());
	}

	std::string FingerprintValidator::ComputeFileSha256(const fs::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
			return "";

		std::ostringstream ss;
		ss << file.rdbuf();
		const std::string s = ss.str();
		return ComputeSha256(s.data(), s.size());
	}

	bool FingerprintValidator::ValidateEngineTree(std::vector<std::string>& outViolations)
	{
		outViolations.clear();
		const fs::path roots[] = {
			fs::current_path() / "Engine" / "src",
			fs::current_path() / "Editor" / "src",
			fs::current_path() / "Runtime" / "src"
		};

		for (const auto& root : roots)
		{
			if (!fs::exists(root))
				continue;

			std::error_code ec;
			for (const auto& entry : fs::recursive_directory_iterator(root, ec))
			{
				if (!entry.is_regular_file())
					continue;

				const auto ext = entry.path().extension().string();
				if (ext == ".obj" || ext == ".lib" || ext == ".dll" || ext == ".pdb" || ext == ".idb" || ext == ".exp")
				{
					outViolations.push_back(entry.path().string());
				}
			}
		}

		// 检查指纹清单 (若存在 engine-fingerprint.sha256 账本)
		const fs::path manifestPath = fs::current_path() / ".we" / "ai" / "engine-fingerprint.sha256";
		if (fs::exists(manifestPath))
		{
			std::ifstream mf(manifestPath);
			std::string line;
			while (std::getline(mf, line))
			{
				if (line.size() < 66 || line[0] == '#') continue;
				const std::string expectedHash = line.substr(0, 64);
				std::string relFile = line.substr(65);
				while (!relFile.empty() && (relFile.front() == ' ' || relFile.front() == '\t')) relFile.erase(0, 1);
				while (!relFile.empty() && (relFile.back() == ' ' || relFile.back() == '\t' || relFile.back() == '\r')) relFile.pop_back();

				const fs::path target = fs::current_path() / fs::u8path(relFile);
				if (!fs::exists(target))
				{
					outViolations.push_back("MISSING: " + relFile);
				}
				else
				{
					const std::string curHash = ComputeFileSha256(target);
					if (curHash != expectedHash)
					{
						outViolations.push_back("MODIFIED: " + relFile);
					}
				}
			}
		}

		return outViolations.empty();
	}
}

