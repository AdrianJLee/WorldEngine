#include "wldpch.h"

#include "World/Core/Sha256.h"

#include <cstring>

namespace World::Crypto
{
	namespace
	{
		constexpr uint32_t kRoundConstants[64] = {
			0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
			0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
			0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
			0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
			0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
			0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
			0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
			0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
			0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
			0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
			0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
		};

		constexpr uint32_t kInitialState[8] = {
			0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
			0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
		};

		inline uint32_t RotateRight(uint32_t value, uint32_t bits)
		{
			return (value >> bits) | (value << (32 - bits));
		}

		void CompressBlock(uint32_t state[8], const uint8_t block[64])
		{
			uint32_t schedule[64];
			for (int index = 0; index < 16; ++index)
			{
				schedule[index] = (static_cast<uint32_t>(block[index * 4]) << 24)
					| (static_cast<uint32_t>(block[index * 4 + 1]) << 16)
					| (static_cast<uint32_t>(block[index * 4 + 2]) << 8)
					| static_cast<uint32_t>(block[index * 4 + 3]);
			}
			for (int index = 16; index < 64; ++index)
			{
				const uint32_t s0 = RotateRight(schedule[index - 15], 7)
					^ RotateRight(schedule[index - 15], 18) ^ (schedule[index - 15] >> 3);
				const uint32_t s1 = RotateRight(schedule[index - 2], 17)
					^ RotateRight(schedule[index - 2], 19) ^ (schedule[index - 2] >> 10);
				schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
			}

			uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
			uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
			for (int index = 0; index < 64; ++index)
			{
				const uint32_t sum1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
				const uint32_t choose = (e & f) ^ (~e & g);
				const uint32_t temp1 = h + sum1 + choose + kRoundConstants[index] + schedule[index];
				const uint32_t sum0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
				const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
				const uint32_t temp2 = sum0 + majority;
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

	Sha256Digest Sha256(const uint8_t* data, size_t size)
	{
		uint32_t state[8];
		std::memcpy(state, kInitialState, sizeof(state));

		size_t offset = 0;
		while (offset + 64 <= size)
		{
			CompressBlock(state, data + offset);
			offset += 64;
		}

		uint8_t tail[128] = {};
		const size_t remaining = size - offset;
		if (remaining > 0)
			std::memcpy(tail, data + offset, remaining);
		tail[remaining] = 0x80;
		const size_t tailSize = remaining + 1 + 8 > 64 ? 128 : 64;
		const uint64_t bitLength = static_cast<uint64_t>(size) * 8ull;
		for (int index = 0; index < 8; ++index)
			tail[tailSize - 1 - index] = static_cast<uint8_t>((bitLength >> (index * 8)) & 0xFF);
		CompressBlock(state, tail);
		if (tailSize == 128)
			CompressBlock(state, tail + 64);

		Sha256Digest digest;
		for (int index = 0; index < 8; ++index)
		{
			digest.Bytes[index * 4] = static_cast<uint8_t>((state[index] >> 24) & 0xFF);
			digest.Bytes[index * 4 + 1] = static_cast<uint8_t>((state[index] >> 16) & 0xFF);
			digest.Bytes[index * 4 + 2] = static_cast<uint8_t>((state[index] >> 8) & 0xFF);
			digest.Bytes[index * 4 + 3] = static_cast<uint8_t>(state[index] & 0xFF);
		}
		return digest;
	}

	Sha256Digest Sha256(const std::vector<uint8_t>& bytes)
	{
		return Sha256(bytes.data(), bytes.size());
	}

	std::string Sha256Hex(const Sha256Digest& digest)
	{
		static const char* const digits = "0123456789abcdef";
		std::string hex;
		hex.reserve(64);
		for (const uint8_t byte : digest.Bytes)
		{
			hex.push_back(digits[(byte >> 4) & 0xF]);
			hex.push_back(digits[byte & 0xF]);
		}
		return hex;
	}

	std::string Sha256Hex(const std::vector<uint8_t>& bytes)
	{
		return Sha256Hex(Sha256(bytes));
	}
}
