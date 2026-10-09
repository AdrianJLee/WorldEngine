#pragma once

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <string>

namespace World::Level::Script
{
	struct ChunkEventPayload
	{
		uint64_t ChunkID{ 0 };
		uint32_t SubspaceID{ 0 };
		bool IsLoaded{ false };
	};

	struct DataLayerEventPayload
	{
		std::string LayerName;
		bool IsActive{ false };
	};

	struct FastTravelRequest
	{
		uint64_t EntityUUID{ 0 };
		glm::vec3 TargetPosition{ 0.0f };
		bool RequireCollisionReady{ true };
		bool RequireNavMeshReady{ true };
	};

	using FastTravelCompleteCallback = std::function<void(bool success)>;
	using DataLayerToggleCallback = std::function<void(const std::string& layerName, bool active)>;
}
