#include "wldpch.h"
#include "World/Ai/VfxCommands.h"
#include "World/Renderer/ParticleAsset.h"
#include "World/Renderer/ParticleSystem.h"

#include <sstream>
#include <iomanip>
#include <filesystem>

namespace World::Ai
{
	static std::string EscapeJson(const std::string& s)
	{
		std::ostringstream o;
		for (char c : s)
		{
			if (c == '"') o << "\"";
			else if (c == '\\') o << "\\\\";
			else if (c == '\b') o << "\\b";
			else if (c == '\f') o << "\\f";
			else if (c == '\n') o << "\\n";
			else if (c == '\r') o << "\\r";
			else if (c == '\t') o << "\\t";
			else if ('\x00' <= c && c <= '\x1f')
				o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
			else o << c;
		}
		return o.str();
	}

	void VfxCommands::RegisterCommands(CommandRegistry& reg)
	{
		// 1. vfx.list
		{
			CommandDescriptor cmd;
			cmd.name = "vfx.list";
			cmd.description = "List all tracked VFX particle assets (.wvfx / .wparticle) and active particle pools.";
			cmd.effect = CommandEffect::Read;
			cmd.scope = CommandScope::Both;
			cmd.owner = "engine";
			cmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"root\":{\"type\":\"string\"}}}";
			cmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"assets\":{\"type\":\"array\"},\"active_pools\":{\"type\":\"number\"}}}";
			cmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
			{
				auto& registry = World::ParticleAssetRegistry::Get();
				std::ostringstream oss;
				oss << "{\"assets\":[";
				bool first = true;
				const std::string root = args.count("root") ? args.at("root") : "";
				if (!root.empty() && std::filesystem::exists(root))
				{
					for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
					{
						if (entry.is_regular_file())
						{
							const auto ext = entry.path().extension().string();
							if (ext == ".wvfx" || ext == ".wparticle")
							{
								if (!first) oss << ",";
								first = false;
								oss << "\"" << EscapeJson(entry.path().generic_string()) << "\"";
							}
						}
					}
				}
				oss << "],\"registered_assets\":" << registry.Count()
				    << ",\"active_oneshots\":" << World::GetGlobalParticleSystem().GetOneShots().size()
				    << "}";
				result = oss.str();
				return true;
			};
			reg.RegisterCommand(std::move(cmd));
		}

		// 2. vfx.spawn
		{
			CommandDescriptor cmd;
			cmd.name = "vfx.spawn";
			cmd.description = "Spawn an entity-less one-shot particle effect at given coordinates.";
			cmd.effect = CommandEffect::Write;
			cmd.scope = CommandScope::Both;
			cmd.owner = "engine";
			cmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"asset\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"count\":{\"type\":\"number\"}}}";
			cmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"success\":{\"type\":\"boolean\"},\"spawned_count\":{\"type\":\"number\"}}}";
			cmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
			{
				float x = args.count("x") ? std::stof(args.at("x")) : 0.0f;
				float y = args.count("y") ? std::stof(args.at("y")) : 0.0f;
				float z = args.count("z") ? std::stof(args.at("z")) : 0.0f;
				uint32_t count = args.count("count") ? static_cast<uint32_t>(std::stoul(args.at("count"))) : 32u;

				World::ParticleEmitterConfig comp;
				comp.MaxParticles = count;
				comp.Speed = 4.0f;
				comp.Lifetime = 1.0f;
				comp.EmissionRate = 0.0f;
				comp.IsPlaying = 1;
				comp.EmitterType = ParticleEmitterType::Sphere;

				World::ParticleMaterialSettings mat;
				const std::string assetPath = args.count("asset") ? args.at("asset") : "";
				if (!assetPath.empty())
				{
					if (const auto* asset = World::ParticleAssetRegistry::Get().GetAssetByPath(assetPath))
					{
						if (!asset->Emitters.empty())
						{
							comp.Lifetime = asset->Emitters[0].Lifetime;
							comp.Speed = asset->Emitters[0].Speed;
							mat = asset->Emitters[0].Material;
						}
					}
				}

				World::GetGlobalParticleSystem().SpawnOneShot(comp, mat, glm::vec3(x, y, z));
				result = "{\"success\":true,\"spawned_count\":" + std::to_string(count) + "}";
				return true;
			};
			reg.RegisterCommand(std::move(cmd));
		}

		// 3. vfx.stats
		{
			CommandDescriptor cmd;
			cmd.name = "vfx.stats";
			cmd.description = "Return runtime particle system telemetry including active particle counts and transient pools.";
			cmd.effect = CommandEffect::Read;
			cmd.scope = CommandScope::Both;
			cmd.owner = "engine";
			cmd.paramsSchema = "{\"type\":\"object\"}";
			cmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"active_oneshots\":{\"type\":\"number\"},\"active_particles\":{\"type\":\"number\"}}}";
			cmd.handler = [](const std::map<std::string, std::string>& /*args*/, std::string& result, std::string& /*error*/)
			{
				auto& sys = World::GetGlobalParticleSystem();
				size_t oneshotCount = sys.GetOneShots().size();
				size_t activeParticles = sys.ActiveOneShotCount();

				std::ostringstream oss;
				oss << "{\"active_oneshots\":" << oneshotCount
				    << ",\"active_particles\":" << activeParticles
				    << ",\"pool_count\":" << sys.PoolCount()
				    << "}";
				result = oss.str();
				return true;
			};
			reg.RegisterCommand(std::move(cmd));
		}

		// 4. vfx.preview
		{
			CommandDescriptor cmd;
			cmd.name = "vfx.preview";
			cmd.description = "Simulate a deterministic particle timeline forward for N frames and return step curve.";
			cmd.effect = CommandEffect::Read;
			cmd.scope = CommandScope::Both;
			cmd.owner = "engine";
			cmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"frames\":{\"type\":\"number\"},\"dt\":{\"type\":\"number\"},\"seed\":{\"type\":\"number\"}}}";
			cmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"deterministic\":{\"type\":\"boolean\"},\"final_count\":{\"type\":\"number\"}}}";
			cmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
			{
				uint32_t frames = args.count("frames") ? static_cast<uint32_t>(std::stoul(args.at("frames"))) : 30u;
				float dt = args.count("dt") ? std::stof(args.at("dt")) : 0.016f;
				uint32_t seed = args.count("seed") ? static_cast<uint32_t>(std::stoul(args.at("seed"))) : 1337u;

				World::ParticlePool pool(128);
				pool.SetSeed(seed);

				World::ParticleEmitterConfig comp;
				comp.Lifetime = 0.5f;
				comp.EmissionRate = 60.0f;
				comp.Speed = 5.0f;
				comp.IsPlaying = 1;

				for (uint32_t f = 0; f < frames; ++f)
				{
					pool.Update(dt, comp, glm::vec3(0.0f));
				}

				std::ostringstream oss;
				oss << "{\"deterministic\":true,\"seed\":" << seed
				    << ",\"frames\":" << frames
				    << ",\"final_active_count\":" << pool.ActiveCount()
				    << "}";
				result = oss.str();
				return true;
			};
			reg.RegisterCommand(std::move(cmd));
		}

		// 5. vfx.validate
		{
			CommandDescriptor cmd;
			cmd.name = "vfx.validate";
			cmd.description = "Perform static sanity checks on a particle asset specification.";
			cmd.effect = CommandEffect::Read;
			cmd.scope = CommandScope::Both;
			cmd.owner = "engine";
			cmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}";
			cmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"valid\":{\"type\":\"boolean\"},\"error\":{\"type\":\"string\"}}}";
			cmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
			{
				const std::string path = args.count("path") ? args.at("path") : "";
				if (path.empty())
				{
					error = "Missing required argument 'path'";
					return false;
				}

				World::ParticleAsset asset;
				if (!World::ParticleAssetSerializer::Deserialize(path, asset))
				{
					result = "{\"valid\":false,\"error\":\"Failed to parse YAML asset\"}";
					return true;
				}

				if (asset.Emitters.empty())
				{
					result = "{\"valid\":false,\"error\":\"Asset has no emitters\"}";
					return true;
				}

				for (const auto& em : asset.Emitters)
				{
					if (em.MaxParticles > 20000)
					{
						result = "{\"valid\":false,\"error\":\"MaxParticles exceeds 20000 safety budget\"}";
						return true;
					}
					if (em.Lifetime <= 0.0f)
					{
						result = "{\"valid\":false,\"error\":\"Lifetime must be greater than 0\"}";
						return true;
					}
				}

				result = "{\"valid\":true,\"emitters_count\":" + std::to_string(asset.Emitters.size()) + "}";
				return true;
			};
			reg.RegisterCommand(std::move(cmd));
		}
	}
}
