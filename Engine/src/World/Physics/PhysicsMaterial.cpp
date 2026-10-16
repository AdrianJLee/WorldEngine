#include "wldpch.h"
#include "World/Physics/PhysicsMaterial.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <string>

namespace World
{
	namespace
	{
		std::string CombineModeToString(PhysMatCombineMode mode)
		{
			switch (mode)
			{
			case PhysMatCombineMode::Average:  return "Average";
			case PhysMatCombineMode::Minimum:  return "Minimum";
			case PhysMatCombineMode::Multiply: return "Multiply";
			case PhysMatCombineMode::Maximum:  return "Maximum";
			default:                           return "Average";
			}
		}

		PhysMatCombineMode CombineModeFromString(const std::string& str)
		{
			std::string s = str;
			std::transform(s.begin(), s.end(), s.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (s == "minimum" || s == "min" || s == "1") return PhysMatCombineMode::Minimum;
			if (s == "multiply" || s == "mul" || s == "2") return PhysMatCombineMode::Multiply;
			if (s == "maximum" || s == "max" || s == "3") return PhysMatCombineMode::Maximum;
			return PhysMatCombineMode::Average;
		}
	}

	float PhysicsMaterial::Combine(PhysMatCombineMode mode, float a, float b)
	{
		switch (mode)
		{
		case PhysMatCombineMode::Average:
			return (a + b) * 0.5f;
		case PhysMatCombineMode::Minimum:
			return std::min(a, b);
		case PhysMatCombineMode::Multiply:
			return a * b;
		case PhysMatCombineMode::Maximum:
			return std::max(a, b);
		default:
			return (a + b) * 0.5f;
		}
	}

	bool PhysicsMaterial::SaveToFile(const std::string& filepath) const
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "Id" << YAML::Value << Id;
		out << YAML::Key << "Friction" << YAML::Value << Friction;
		out << YAML::Key << "Restitution" << YAML::Value << Restitution;
		out << YAML::Key << "FrictionCombine" << YAML::Value << CombineModeToString(FrictionCombine);
		out << YAML::Key << "RestitutionCombine" << YAML::Value << CombineModeToString(RestitutionCombine);
		out << YAML::EndMap;

		std::ofstream fout(filepath);
		if (!fout.is_open())
			return false;
		fout << out.c_str();
		return true;
	}

	bool PhysicsMaterial::LoadFromFile(const std::string& filepath, PhysicsMaterial& outMaterial)
	{
		try
		{
			YAML::Node root = YAML::LoadFile(filepath);
			if (!root || !root.IsMap())
				return false;

			if (root["Id"])
				outMaterial.Id = root["Id"].as<std::string>();
			if (root["Friction"])
				outMaterial.Friction = root["Friction"].as<float>();
			if (root["Restitution"])
				outMaterial.Restitution = root["Restitution"].as<float>();
			if (root["FrictionCombine"] && root["FrictionCombine"].IsScalar())
				outMaterial.FrictionCombine = CombineModeFromString(root["FrictionCombine"].as<std::string>());
			if (root["RestitutionCombine"] && root["RestitutionCombine"].IsScalar())
				outMaterial.RestitutionCombine = CombineModeFromString(root["RestitutionCombine"].as<std::string>());
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
}
