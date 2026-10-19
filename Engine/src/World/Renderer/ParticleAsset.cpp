#include "wldpch.h"
#include "World/Renderer/ParticleAsset.h"
#include "World/Utils/Paths.h"
#include <filesystem>

#include <yaml-cpp/yaml.h>
#include <fstream>
#include <sstream>
#include <functional>

namespace World
{
	static std::string BlendModeToString(ParticleBlendMode mode)
	{
		switch (mode)
		{
		case ParticleBlendMode::Additive: return "Additive";
		case ParticleBlendMode::Multiply: return "Multiply";
		case ParticleBlendMode::Opaque:   return "Opaque";
		case ParticleBlendMode::AlphaBlend:
		default: return "AlphaBlend";
		}
	}

	static ParticleBlendMode StringToBlendMode(const std::string& str)
	{
		if (str == "Additive") return ParticleBlendMode::Additive;
		if (str == "Multiply") return ParticleBlendMode::Multiply;
		if (str == "Opaque")   return ParticleBlendMode::Opaque;
		return ParticleBlendMode::AlphaBlend;
	}

		static std::string FacingModeToString(ParticleRenderMode mode)
	{
		switch (mode)
		{
		case ParticleRenderMode::StretchedVelocity: return "StretchedVelocity";
		case ParticleRenderMode::HorizontalQuad:    return "HorizontalQuad";
		case ParticleRenderMode::Quad2D:            return "Quad2D";
		case ParticleRenderMode::Billboard:
		default: return "Billboard";
		}
	}

	static ParticleRenderMode StringToFacingMode(const std::string& str)
	{
		if (str == "StretchedVelocity") return ParticleRenderMode::StretchedVelocity;
		if (str == "HorizontalQuad")    return ParticleRenderMode::HorizontalQuad;
		if (str == "Quad2D")            return ParticleRenderMode::Quad2D;
		return ParticleRenderMode::Billboard;
	}

	static std::string EmitterTypeToString(ParticleEmitterType type)
	{
		switch (type)
		{
		case ParticleEmitterType::Box:    return "Box";
		case ParticleEmitterType::Sphere: return "Sphere";
		case ParticleEmitterType::Cone:   return "Cone";
		case ParticleEmitterType::Point:
		default: return "Point";
		}
	}

	static ParticleEmitterType StringToEmitterType(const std::string& str)
	{
		if (str == "Box")    return ParticleEmitterType::Box;
		if (str == "Sphere") return ParticleEmitterType::Sphere;
		if (str == "Cone")   return ParticleEmitterType::Cone;
		return ParticleEmitterType::Point;
	}

	static std::string CollisionModeToString(ParticleCollisionMode mode)
	{
		switch (mode)
		{
		case ParticleCollisionMode::Plane:        return "Plane";
		case ParticleCollisionMode::WorldRaycast: return "WorldRaycast";
		case ParticleCollisionMode::None:
		default: return "None";
		}
	}

	static ParticleCollisionMode StringToCollisionMode(const std::string& str)
	{
		if (str == "Plane")        return ParticleCollisionMode::Plane;
		if (str == "WorldRaycast") return ParticleCollisionMode::WorldRaycast;
		return ParticleCollisionMode::None;
	}

	static void EmitEmitterLayer(YAML::Emitter& out, const ParticleEmitterLayer& em)
	{
		out << YAML::BeginMap;
		out << YAML::Key << "Name" << YAML::Value << em.Name;
		out << YAML::Key << "MaxParticles" << YAML::Value << em.MaxParticles;
		out << YAML::Key << "Lifetime" << YAML::Value << em.Lifetime;
		out << YAML::Key << "EmissionRate" << YAML::Value << em.EmissionRate;
		out << YAML::Key << "Enabled" << YAML::Value << em.Enabled;

		out << YAML::Key << "Emitter" << YAML::Value << YAML::BeginMap;
		out << YAML::Key << "Type" << YAML::Value << EmitterTypeToString(em.EmitterType);
		out << YAML::Key << "LockToXY" << YAML::Value << em.LockToXY;
		out << YAML::Key << "MeshPath" << YAML::Value << em.MeshPath;
		out << YAML::EndMap;

		out << YAML::Key << "Material" << YAML::Value << YAML::BeginMap;
		out << YAML::Key << "TexturePath" << YAML::Value << em.Material.TexturePath;
		out << YAML::Key << "MaterialPath" << YAML::Value << em.Material.MaterialPath;
		out << YAML::Key << "BlendMode" << YAML::Value << BlendModeToString(em.Material.BlendMode);
		out << YAML::Key << "FacingMode" << YAML::Value << FacingModeToString(em.FacingMode);
		out << YAML::Key << "EmissiveBoost" << YAML::Value << em.Material.EmissiveBoost;
		out << YAML::Key << "SoftParticle" << YAML::Value << em.Material.SoftParticlesEnabled;
		out << YAML::Key << "FadeDistance" << YAML::Value << em.Material.SoftParticleFadeDistance;
		out << YAML::Key << "SubUV" << YAML::Value << YAML::BeginMap;
		out << YAML::Key << "TilesX" << YAML::Value << em.Material.SubUVTilesX;
		out << YAML::Key << "TilesY" << YAML::Value << em.Material.SubUVTilesY;
		out << YAML::Key << "FPS" << YAML::Value << em.Material.SubUVFrameRate;
		out << YAML::Key << "RandomStart" << YAML::Value << em.Material.SubUVRandomStartFrame;
		out << YAML::EndMap;
		out << YAML::EndMap;

		out << YAML::Key << "Motion" << YAML::Value << YAML::BeginMap;
		out << YAML::Key << "Speed" << YAML::Value << em.Speed;
		out << YAML::Key << "SpeedVariance" << YAML::Value << em.SpeedVariance;
		out << YAML::Key << "Gravity" << YAML::Value << YAML::Flow << YAML::BeginSeq
			<< em.Gravity.x << em.Gravity.y << em.Gravity.z << YAML::EndSeq;
		out << YAML::Key << "LinearDrag" << YAML::Value << em.LinearDrag;
		out << YAML::Key << "VortexStrength" << YAML::Value << em.VortexStrength;
		out << YAML::Key << "VortexAxis" << YAML::Value << YAML::Flow << YAML::BeginSeq
			<< em.VortexAxis.x << em.VortexAxis.y << em.VortexAxis.z << YAML::EndSeq;
		out << YAML::Key << "TurbulenceStrength" << YAML::Value << em.TurbulenceStrength;
		out << YAML::Key << "TurbulenceFrequency" << YAML::Value << em.TurbulenceFrequency;
		out << YAML::EndMap;

		out << YAML::Key << "Evolution" << YAML::Value << YAML::BeginMap;
		out << YAML::Key << "SizeStart" << YAML::Value << em.SizeStart;
		out << YAML::Key << "SizeEnd" << YAML::Value << em.SizeEnd;
		out << YAML::Key << "ColorStart" << YAML::Value << YAML::Flow << YAML::BeginSeq
			<< em.ColorStart.r << em.ColorStart.g << em.ColorStart.b << em.ColorStart.a << YAML::EndSeq;
		out << YAML::Key << "ColorEnd" << YAML::Value << YAML::Flow << YAML::BeginSeq
			<< em.ColorEnd.r << em.ColorEnd.g << em.ColorEnd.b << em.ColorEnd.a << YAML::EndSeq;
		out << YAML::EndMap;

		out << YAML::Key << "Collision" << YAML::Value << YAML::BeginMap;
		out << YAML::Key << "Mode" << YAML::Value << CollisionModeToString(em.Collision.Mode);
		out << YAML::Key << "Restitution" << YAML::Value << em.Collision.Restitution;
		out << YAML::Key << "Friction" << YAML::Value << em.Collision.Friction;
		out << YAML::Key << "KillOnHit" << YAML::Value << em.Collision.KillOnHit;
		out << YAML::Key << "EnableEvents" << YAML::Value << em.Collision.EnableEvents;
		out << YAML::Key << "PlaneOrigin" << YAML::Value << YAML::Flow << YAML::BeginSeq
			<< em.Collision.PlaneOrigin.x << em.Collision.PlaneOrigin.y << em.Collision.PlaneOrigin.z << YAML::EndSeq;
		out << YAML::Key << "PlaneNormal" << YAML::Value << YAML::Flow << YAML::BeginSeq
			<< em.Collision.PlaneNormal.x << em.Collision.PlaneNormal.y << em.Collision.PlaneNormal.z << YAML::EndSeq;
		out << YAML::EndMap;

		if (!em.Bursts.empty())
		{
			out << YAML::Key << "Bursts" << YAML::Value << YAML::BeginSeq;
			for (const auto& b : em.Bursts)
			{
				out << YAML::BeginMap;
				out << YAML::Key << "Time" << YAML::Value << b.Time;
				out << YAML::Key << "Count" << YAML::Value << b.Count;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;
		}
		out << YAML::EndMap;
	}

	static void ParseEmitterLayer(const YAML::Node& node, ParticleEmitterLayer& em)
	{
		if (node["Name"]) em.Name = node["Name"].as<std::string>();
		if (node["MaxParticles"]) em.MaxParticles = node["MaxParticles"].as<uint32_t>();
		if (node["Lifetime"]) em.Lifetime = node["Lifetime"].as<float>();
		if (node["EmissionRate"]) em.EmissionRate = node["EmissionRate"].as<float>();
		if (node["Enabled"]) em.Enabled = node["Enabled"].as<bool>();

		if (const auto emNode = node["Emitter"])
		{
			if (emNode["Type"]) em.EmitterType = StringToEmitterType(emNode["Type"].as<std::string>());
			if (emNode["LockToXY"]) em.LockToXY = emNode["LockToXY"].as<bool>();
		}

		if (node["MeshPath"]) em.MeshPath = node["MeshPath"].as<std::string>();

		if (const auto matNode = node["Material"])
		{
			if (matNode["TexturePath"]) em.Material.TexturePath = matNode["TexturePath"].as<std::string>();
			if (matNode["MaterialPath"]) em.Material.MaterialPath = matNode["MaterialPath"].as<std::string>();
			if (matNode["BlendMode"]) em.Material.BlendMode = StringToBlendMode(matNode["BlendMode"].as<std::string>());
			if (matNode["FacingMode"]) em.FacingMode = StringToFacingMode(matNode["FacingMode"].as<std::string>());
			em.Material.FacingMode = em.FacingMode;
			if (matNode["EmissiveBoost"]) em.Material.EmissiveBoost = matNode["EmissiveBoost"].as<float>();
			if (matNode["SoftParticle"]) em.Material.SoftParticlesEnabled = matNode["SoftParticle"].as<bool>();
			if (matNode["FadeDistance"]) em.Material.SoftParticleFadeDistance = matNode["FadeDistance"].as<float>();

			if (const auto subNode = matNode["SubUV"])
			{
				if (subNode["TilesX"]) em.Material.SubUVTilesX = subNode["TilesX"].as<uint32_t>();
				if (subNode["TilesY"]) em.Material.SubUVTilesY = subNode["TilesY"].as<uint32_t>();
				if (subNode["FPS"]) em.Material.SubUVFrameRate = subNode["FPS"].as<float>();
				if (subNode["RandomStart"]) em.Material.SubUVRandomStartFrame = subNode["RandomStart"].as<bool>();
			}
		}

		if (const auto motNode = node["Motion"])
		{
			if (motNode["Speed"]) em.Speed = motNode["Speed"].as<float>();
			if (motNode["SpeedVariance"]) em.SpeedVariance = motNode["SpeedVariance"].as<float>();
			if (motNode["LinearDrag"]) em.LinearDrag = motNode["LinearDrag"].as<float>();
			if (motNode["VortexStrength"]) em.VortexStrength = motNode["VortexStrength"].as<float>();
			if (motNode["VortexAxis"] && motNode["VortexAxis"].IsSequence() && motNode["VortexAxis"].size() >= 3)
			{
				em.VortexAxis = glm::vec3(
					motNode["VortexAxis"][0].as<float>(),
					motNode["VortexAxis"][1].as<float>(),
					motNode["VortexAxis"][2].as<float>()
				);
			}
			if (motNode["TurbulenceStrength"]) em.TurbulenceStrength = motNode["TurbulenceStrength"].as<float>();
			if (motNode["TurbulenceFrequency"]) em.TurbulenceFrequency = motNode["TurbulenceFrequency"].as<float>();
			if (motNode["Gravity"] && motNode["Gravity"].IsSequence() && motNode["Gravity"].size() >= 3)
			{
				em.Gravity = glm::vec3(
					motNode["Gravity"][0].as<float>(),
					motNode["Gravity"][1].as<float>(),
					motNode["Gravity"][2].as<float>()
				);
			}
		}

		if (const auto evoNode = node["Evolution"])
		{
			if (evoNode["SizeStart"]) em.SizeStart = evoNode["SizeStart"].as<float>();
			if (evoNode["SizeEnd"]) em.SizeEnd = evoNode["SizeEnd"].as<float>();
			if (evoNode["ColorStart"] && evoNode["ColorStart"].IsSequence() && evoNode["ColorStart"].size() >= 4)
			{
				em.ColorStart = glm::vec4(
					evoNode["ColorStart"][0].as<float>(),
					evoNode["ColorStart"][1].as<float>(),
					evoNode["ColorStart"][2].as<float>(),
					evoNode["ColorStart"][3].as<float>()
				);
			}
			if (evoNode["ColorEnd"] && evoNode["ColorEnd"].IsSequence() && evoNode["ColorEnd"].size() >= 4)
			{
				em.ColorEnd = glm::vec4(
					evoNode["ColorEnd"][0].as<float>(),
					evoNode["ColorEnd"][1].as<float>(),
					evoNode["ColorEnd"][2].as<float>(),
					evoNode["ColorEnd"][3].as<float>()
				);
			}
		}

		if (const auto colNode = node["Collision"])
		{
			if (colNode["Mode"]) em.Collision.Mode = StringToCollisionMode(colNode["Mode"].as<std::string>());
			if (colNode["Restitution"]) em.Collision.Restitution = colNode["Restitution"].as<float>();
			if (colNode["Friction"]) em.Collision.Friction = colNode["Friction"].as<float>();
			if (colNode["KillOnHit"]) em.Collision.KillOnHit = colNode["KillOnHit"].as<bool>();
			if (colNode["EnableEvents"]) em.Collision.EnableEvents = colNode["EnableEvents"].as<bool>();
			if (colNode["PlaneOrigin"] && colNode["PlaneOrigin"].IsSequence() && colNode["PlaneOrigin"].size() >= 3)
			{
				em.Collision.PlaneOrigin = glm::vec3(
					colNode["PlaneOrigin"][0].as<float>(),
					colNode["PlaneOrigin"][1].as<float>(),
					colNode["PlaneOrigin"][2].as<float>()
				);
			}
			if (colNode["PlaneNormal"] && colNode["PlaneNormal"].IsSequence() && colNode["PlaneNormal"].size() >= 3)
			{
				em.Collision.PlaneNormal = glm::vec3(
					colNode["PlaneNormal"][0].as<float>(),
					colNode["PlaneNormal"][1].as<float>(),
					colNode["PlaneNormal"][2].as<float>()
				);
			}
		}

		if (const auto burstsNode = node["Bursts"])
		{
			em.Bursts.clear();
			for (const auto& bNode : burstsNode)
			{
				ParticleBurst b;
				if (bNode["Time"]) b.Time = bNode["Time"].as<float>();
				if (bNode["Count"]) b.Count = bNode["Count"].as<uint32_t>();
				em.Bursts.push_back(b);
			}
		}
	}

	std::string ParticleAssetSerializer::SerializeToString(const ParticleAsset& asset)
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "FormatVersion" << YAML::Value << 1;
		out << YAML::Key << "Name" << YAML::Value << asset.Name;
		out << YAML::Key << "Guid" << YAML::Value << asset.Guid;
		out << YAML::Key << "Seed" << YAML::Value << asset.Seed;
		out << YAML::Key << "Duration" << YAML::Value << asset.Duration;
		out << YAML::Key << "Looping" << YAML::Value << asset.Looping;
		out << YAML::Key << "WarmupTime" << YAML::Value << asset.WarmupTime;

		out << YAML::Key << "LOD" << YAML::Value << YAML::BeginMap;
		out << YAML::Key << "CullDistance" << YAML::Value << asset.CullDistance;
		out << YAML::Key << "PauseWhenOffscreen" << YAML::Value << asset.PauseWhenOffscreen;
		out << YAML::EndMap;

		if (!asset.UserParamsFloat.empty())
		{
			out << YAML::Key << "UserParams" << YAML::Value << YAML::BeginMap;
			for (const auto& [k, v] : asset.UserParamsFloat)
				out << YAML::Key << k << YAML::Value << v;
			out << YAML::EndMap;
		}

		out << YAML::Key << "Emitters" << YAML::Value << YAML::BeginSeq;
		for (const auto& em : asset.Emitters)
		{
			EmitEmitterLayer(out, em);
		}
		out << YAML::EndSeq;

		out << YAML::EndMap;
		return out.c_str();
	}

	bool ParticleAssetSerializer::Serialize(const ParticleAsset& asset, const std::string& filepath)
	{
		std::ofstream fout(filepath);
		if (!fout.is_open()) return false;
		fout << SerializeToString(asset);
		return true;
	}

	bool ParticleAssetSerializer::DeserializeFromString(const std::string& yamlString, ParticleAsset& outAsset)
	{
		YAML::Node data;
		try
		{
			data = YAML::Load(yamlString);
		}
		catch (...)
		{
			return false;
		}

		if (!data.IsMap()) return false;

		if (data["Name"]) outAsset.Name = data["Name"].as<std::string>();
		if (data["Guid"]) outAsset.Guid = data["Guid"].as<uint64_t>();
		if (data["Seed"]) outAsset.Seed = data["Seed"].as<uint32_t>();
		if (data["Duration"]) outAsset.Duration = data["Duration"].as<float>();
		if (data["Looping"]) outAsset.Looping = data["Looping"].as<bool>();
		if (data["WarmupTime"]) outAsset.WarmupTime = data["WarmupTime"].as<float>();

		if (const auto lodNode = data["LOD"])
		{
			if (lodNode["CullDistance"]) outAsset.CullDistance = lodNode["CullDistance"].as<float>();
			if (lodNode["PauseWhenOffscreen"]) outAsset.PauseWhenOffscreen = lodNode["PauseWhenOffscreen"].as<bool>();
		}

		if (const auto upNode = data["UserParams"])
		{
			if (upNode.IsMap())
			{
				for (auto it = upNode.begin(); it != upNode.end(); ++it)
				{
					if (it->second.IsScalar())
					{
						outAsset.UserParamsFloat[it->first.as<std::string>()] = it->second.as<float>();
					}
				}
			}
		}

		outAsset.Emitters.clear();

		// Case A: Multi-emitter sequence
		if (const auto emittersNode = data["Emitters"])
		{
			if (emittersNode.IsSequence())
			{
				for (const auto& node : emittersNode)
				{
					ParticleEmitterLayer em;
					ParseEmitterLayer(node, em);
					outAsset.Emitters.push_back(std::move(em));
				}
			}
		}
		// Case B: Backward compatibility for single-emitter flat layout
		else if (data["Lifetime"] || data["EmissionRate"] || data["Material"])
		{
			ParticleEmitterLayer em;
			ParseEmitterLayer(data, em);
			if (em.Name.empty() || em.Name == "Emitter")
				em.Name = outAsset.Name.empty() ? "Main" : outAsset.Name;
			outAsset.Emitters.push_back(std::move(em));
		}

		return true;
	}

	bool ParticleAssetSerializer::Deserialize(const std::string& filepath, ParticleAsset& outAsset)
	{
		std::ifstream stream(filepath);
		if (!stream.is_open()) return false;
		std::stringstream strStream;
		strStream << stream.rdbuf();
		outAsset.AssetPath = filepath;
		return DeserializeFromString(strStream.str(), outAsset);
	}

	ParticleAssetRegistry& ParticleAssetRegistry::Get()
	{
		static ParticleAssetRegistry s_Instance;
		return s_Instance;
	}

	std::filesystem::path ResolveVfxDiskPath(const std::string& path)
	{
		std::error_code ec;
		std::filesystem::path p(path);
		if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec))
			return std::filesystem::absolute(p);

		// Check relative to AssetRoot
		const auto& assetRoot = World::Paths::AssetRoot();
		if (!assetRoot.empty())
		{
			std::filesystem::path fromAssetRoot = assetRoot / p;
			if (std::filesystem::exists(fromAssetRoot, ec) && std::filesystem::is_regular_file(fromAssetRoot, ec))
				return std::filesystem::absolute(fromAssetRoot);
		}

		// Check stripping "assets/"
		std::string s = p.generic_string();
		if (s.rfind("assets/", 0) == 0)
		{
			std::string sub = s.substr(7);
			std::filesystem::path fromAssetRoot = assetRoot / sub;
			if (std::filesystem::exists(fromAssetRoot, ec) && std::filesystem::is_regular_file(fromAssetRoot, ec))
				return std::filesystem::absolute(fromAssetRoot);
		}

		// Check projects/assets
		std::filesystem::path fromProj = std::filesystem::current_path() / "projects" / "assets" / p;
		if (std::filesystem::exists(fromProj, ec) && std::filesystem::is_regular_file(fromProj, ec))
			return std::filesystem::absolute(fromProj);

		// Check relative to EngineContentRoot
		const auto& engineRoot = World::Paths::EngineContentRoot();
		if (!engineRoot.empty())
		{
			std::filesystem::path fromEngine = engineRoot / p;
			if (std::filesystem::exists(fromEngine, ec) && std::filesystem::is_regular_file(fromEngine, ec))
				return std::filesystem::absolute(fromEngine);

			if (s.rfind("engine/", 0) == 0 || s.rfind("Engine/", 0) == 0)
			{
				std::string sub = s.substr(7);
				std::filesystem::path fromSub = engineRoot / sub;
				if (std::filesystem::exists(fromSub, ec) && std::filesystem::is_regular_file(fromSub, ec))
					return std::filesystem::absolute(fromSub);
			}
		}

		// Check templates/project-example/assets
		std::filesystem::path fromTpl = std::filesystem::current_path() / "templates" / "project-example" / "assets" / p;
		if (std::filesystem::exists(fromTpl, ec) && std::filesystem::is_regular_file(fromTpl, ec))
			return std::filesystem::absolute(fromTpl);

		return p;
	}

	uint64_t ParticleAssetRegistry::RegisterAsset(const std::string& filepath)
	{
		auto it = m_PathToGuid.find(filepath);
		if (it != m_PathToGuid.end())
			return it->second;

		std::filesystem::path diskPath = ResolveVfxDiskPath(filepath);
		ParticleAsset asset;
		if (!ParticleAssetSerializer::Deserialize(diskPath.string(), asset))
			return 0;

		uint64_t guid = asset.Guid != 0 ? asset.Guid : std::hash<std::string>{}(filepath);
		asset.Guid = guid;
		asset.AssetPath = filepath;

		m_Assets[guid] = std::move(asset);
		m_PathToGuid[filepath] = guid;
		m_PathToGuid[diskPath.string()] = guid;
		m_PathToGuid[diskPath.generic_string()] = guid;
		return guid;
	}

	uint64_t ParticleAssetRegistry::RegisterAssetFromMemory(const std::string& pathOrName, const ParticleAsset& asset)
	{
		uint64_t guid = asset.Guid != 0 ? asset.Guid : (static_cast<uint64_t>(std::hash<std::string>{}(pathOrName)) & 0x000FFFFFFFFFFFFFULL);
		if (guid == 0) guid = 1;
		ParticleAsset copy = asset;
		copy.Guid = guid;
		copy.AssetPath = pathOrName;
		if (copy.Name.empty())
			copy.Name = pathOrName;
		m_Assets[guid] = std::move(copy);
		m_PathToGuid[pathOrName] = guid;
		return guid;
	}

	ParticleAsset* ParticleAssetRegistry::GetAssetMut(uint64_t guid)
	{
		auto it = m_Assets.find(guid);
		if (it != m_Assets.end()) return &it->second;
		return nullptr;
	}

	const ParticleAsset* ParticleAssetRegistry::GetAsset(uint64_t guid) const
	{
		auto it = m_Assets.find(guid);
		return (it != m_Assets.end()) ? &it->second : nullptr;
	}

	const ParticleAsset* ParticleAssetRegistry::GetAssetByPath(const std::string& path) const
	{
		auto it = m_PathToGuid.find(path);
		if (it != m_PathToGuid.end())
			return GetAsset(it->second);

		// Lazy auto-load on first access by path
		uint64_t guid = const_cast<ParticleAssetRegistry*>(this)->RegisterAsset(path);
		if (guid != 0)
			return GetAsset(guid);

		return nullptr;
	}

	void ParticleAssetRegistry::Clear()
	{
		m_Assets.clear();
		m_PathToGuid.clear();
	}
}
