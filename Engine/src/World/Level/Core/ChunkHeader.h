#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

namespace World::Level
{
	constexpr uint32_t ChunkMagic = 0x5743484B; // "WCHK"
	constexpr uint32_t ChunkVersion = 1;

	#pragma pack(push, 16)
	struct alignas(16) ChunkHeader
	{
		uint32_t Magic{ ChunkMagic };
		uint32_t Version{ ChunkVersion };
		uint64_t ChunkID{ 0 };
		uint32_t SubspaceID{ 0 };
		uint32_t Flags{ 0 };

		glm::vec3 BoundsMin{ 0.0f };
		float Padding0{ 0.0f };

		glm::vec3 BoundsMax{ 0.0f };
		float Padding1{ 0.0f };

		uint32_t StaticEntityCount{ 0 };
		uint32_t DynamicSpawnerCount{ 0 };
		uint32_t NavMeshTileIndex{ 0 };
		uint32_t DataLayerMask{ 0 };

		uint64_t BlobOffset{ 0 };
		uint64_t BlobSize{ 0 };
		uint64_t HeaderChecksum{ 0 };

		uint64_t Reserved[4]{ 0, 0, 0, 0 };
	};

	struct alignas(16) StaticEntityRecord
	{
		uint64_t EntityUUID{ 0 };
		uint32_t DataLayerMask{ 1 };
		uint32_t Flags{ 0 };

		glm::vec3 Position{ 0.0f };
		float ScaleX{ 1.0f };

		glm::vec3 RotationEuler{ 0.0f };
		float ScaleY{ 1.0f };

		float ScaleZ{ 1.0f };
		char Name[28]{ 0 };
	};
	#pragma pack(pop)

	static_assert(sizeof(ChunkHeader) == 128, "ChunkHeader must be exactly 128 bytes");
	static_assert(alignof(ChunkHeader) == 16, "ChunkHeader must be 16-byte aligned");
	static_assert(offsetof(ChunkHeader, Magic) == 0, "ChunkHeader::Magic offset mismatch");
	static_assert(offsetof(ChunkHeader, Version) == 4, "ChunkHeader::Version offset mismatch");
	static_assert(offsetof(ChunkHeader, ChunkID) == 8, "ChunkHeader::ChunkID offset mismatch");
	static_assert(offsetof(ChunkHeader, SubspaceID) == 16, "ChunkHeader::SubspaceID offset mismatch");
	static_assert(offsetof(ChunkHeader, Flags) == 20, "ChunkHeader::Flags offset mismatch");
	static_assert(offsetof(ChunkHeader, BoundsMin) == 24, "ChunkHeader::BoundsMin offset mismatch");
	static_assert(offsetof(ChunkHeader, BoundsMax) == 40, "ChunkHeader::BoundsMax offset mismatch");
	static_assert(offsetof(ChunkHeader, StaticEntityCount) == 56, "ChunkHeader::StaticEntityCount offset mismatch");
	static_assert(offsetof(ChunkHeader, DynamicSpawnerCount) == 60, "ChunkHeader::DynamicSpawnerCount offset mismatch");
	static_assert(offsetof(ChunkHeader, NavMeshTileIndex) == 64, "ChunkHeader::NavMeshTileIndex offset mismatch");
	static_assert(offsetof(ChunkHeader, DataLayerMask) == 68, "ChunkHeader::DataLayerMask offset mismatch");
	static_assert(offsetof(ChunkHeader, BlobOffset) == 72, "ChunkHeader::BlobOffset offset mismatch");
	static_assert(offsetof(ChunkHeader, BlobSize) == 80, "ChunkHeader::BlobSize offset mismatch");
	static_assert(offsetof(ChunkHeader, HeaderChecksum) == 88, "ChunkHeader::HeaderChecksum offset mismatch");
	static_assert(offsetof(ChunkHeader, Reserved) == 96, "ChunkHeader::Reserved offset mismatch");

	static_assert(sizeof(StaticEntityRecord) == 80, "StaticEntityRecord must be exactly 80 bytes");
	static_assert(alignof(StaticEntityRecord) == 16, "StaticEntityRecord must be 16-byte aligned");
	static_assert(offsetof(StaticEntityRecord, EntityUUID) == 0, "StaticEntityRecord::EntityUUID offset mismatch");
	static_assert(offsetof(StaticEntityRecord, DataLayerMask) == 8, "StaticEntityRecord::DataLayerMask offset mismatch");
	static_assert(offsetof(StaticEntityRecord, Flags) == 12, "StaticEntityRecord::Flags offset mismatch");
	static_assert(offsetof(StaticEntityRecord, Position) == 16, "StaticEntityRecord::Position offset mismatch");
	static_assert(offsetof(StaticEntityRecord, ScaleX) == 28, "StaticEntityRecord::ScaleX offset mismatch");
	static_assert(offsetof(StaticEntityRecord, RotationEuler) == 32, "StaticEntityRecord::RotationEuler offset mismatch");
	static_assert(offsetof(StaticEntityRecord, ScaleY) == 44, "StaticEntityRecord::ScaleY offset mismatch");
	static_assert(offsetof(StaticEntityRecord, ScaleZ) == 48, "StaticEntityRecord::ScaleZ offset mismatch");
	static_assert(offsetof(StaticEntityRecord, Name) == 52, "StaticEntityRecord::Name offset mismatch");
}
