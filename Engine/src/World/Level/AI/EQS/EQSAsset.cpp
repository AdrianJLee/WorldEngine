#include "wldpch.h"
#include "World/Level/AI/EQS/EQSAsset.h"
#include "World/Level/AI/EQS/EQSQuery.h"
#include "World/Level/AI/EQS/EQSGenerator.h"
#include "World/Level/AI/EQS/EQSTest.h"
#include "World/Level/AI/EQS/EQSContext.h"
#include "World/Utils/Paths.h"
#include "World/Core/Log.h"

#include <yaml-cpp/yaml.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace World::EQS
{
	std::unordered_map<std::string, std::shared_ptr<EQSAsset>> EQSAsset::s_AssetCache;

	namespace
	{
		std::string PurposeToString(TestPurpose p)
		{
			switch (p)
			{
			case TestPurpose::ScoreOnly: return "ScoreOnly";
			case TestPurpose::FilterOnly: return "FilterOnly";
			case TestPurpose::FilterAndScore: return "FilterAndScore";
			default: return "ScoreOnly";
			}
		}

		TestPurpose StringToPurpose(const std::string& str)
		{
			if (str == "FilterOnly") return TestPurpose::FilterOnly;
			if (str == "FilterAndScore") return TestPurpose::FilterAndScore;
			return TestPurpose::ScoreOnly;
		}

		std::string CurveToString(ScoringCurve c)
		{
			switch (c)
			{
			case ScoringCurve::Linear: return "Linear";
			case ScoringCurve::Inverse: return "Inverse";
			case ScoringCurve::ClampedGaussian: return "ClampedGaussian";
			case ScoringCurve::StepThreshold: return "StepThreshold";
			default: return "Linear";
			}
		}

		ScoringCurve StringToCurve(const std::string& str)
		{
			if (str == "Inverse") return ScoringCurve::Inverse;
			if (str == "ClampedGaussian" || str == "Gaussian") return ScoringCurve::ClampedGaussian;
			if (str == "StepThreshold" || str == "Step") return ScoringCurve::StepThreshold;
			return ScoringCurve::Linear;
		}
	}

	std::string EQSAsset::ToYamlString() const
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "FormatVersion" << YAML::Value << Version;
		out << YAML::Key << "AssetType" << YAML::Value << "EQSQuery";
		out << YAML::Key << "Name" << YAML::Value << Name;

		// 1. Generator
		out << YAML::Key << "Generator" << YAML::Value << YAML::BeginMap;
		out << YAML::Key << "Type" << YAML::Value << GeneratorDesc.Type;
		out << YAML::Key << "Radius" << YAML::Value << GeneratorDesc.Radius;
		out << YAML::Key << "Spacing" << YAML::Value << GeneratorDesc.Spacing;
		out << YAML::Key << "InnerRadius" << YAML::Value << GeneratorDesc.InnerRadius;
		out << YAML::Key << "OuterRadius" << YAML::Value << GeneratorDesc.OuterRadius;
		out << YAML::Key << "StepDistance" << YAML::Value << GeneratorDesc.StepDistance;
		out << YAML::Key << "WallOffset" << YAML::Value << GeneratorDesc.WallOffset;
		out << YAML::Key << "ConeAngle" << YAML::Value << GeneratorDesc.ConeAngle;
		out << YAML::Key << "IncludeGrid" << YAML::Value << GeneratorDesc.IncludeGrid;
		out << YAML::Key << "ProjectToTerrain" << YAML::Value << GeneratorDesc.ProjectToTerrain;
		out << YAML::EndMap;

		// 2. Tests Pipeline
		out << YAML::Key << "Tests" << YAML::Value << YAML::BeginSeq;
		for (const auto& t : Tests)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "Name" << YAML::Value << t.Name;
			out << YAML::Key << "Type" << YAML::Value << t.TestType;
			out << YAML::Key << "Context" << YAML::Value << t.ContextType;
			out << YAML::Key << "Purpose" << YAML::Value << PurposeToString(t.Purpose);
			out << YAML::Key << "Curve" << YAML::Value << CurveToString(t.Curve);
			out << YAML::Key << "Weight" << YAML::Value << t.Weight;
			out << YAML::Key << "FilterMin" << YAML::Value << t.FilterMin;
			out << YAML::Key << "FilterMax" << YAML::Value << t.FilterMax;
			out << YAML::Key << "IdealValue" << YAML::Value << t.IdealValue;
			out << YAML::Key << "GaussianSigma" << YAML::Value << t.GaussianSigma;
			if (!t.ScriptPath.empty())
			{
				out << YAML::Key << "ScriptPath" << YAML::Value << t.ScriptPath;
			}
			out << YAML::Key << "Enabled" << YAML::Value << t.Enabled;
			out << YAML::EndMap;
		}
		out << YAML::EndSeq;

		out << YAML::EndMap;
		return std::string(out.c_str());
	}

	bool EQSAsset::FromYamlString(const std::string& yamlStr, std::string* error)
	{
		try
		{
			YAML::Node root = YAML::Load(yamlStr);
			if (!root || !root.IsMap())
			{
				if (error) *error = "Invalid YAML format";
				return false;
			}

			if (root["FormatVersion"]) Version = root["FormatVersion"].as<uint32_t>();
			if (root["Name"]) Name = root["Name"].as<std::string>();

			if (root["Generator"] && root["Generator"].IsMap())
			{
				const auto& g = root["Generator"];
				if (g["Type"]) GeneratorDesc.Type = g["Type"].as<std::string>();
				if (g["Radius"]) GeneratorDesc.Radius = g["Radius"].as<float>();
				if (g["Spacing"]) GeneratorDesc.Spacing = g["Spacing"].as<float>();
				if (g["InnerRadius"]) GeneratorDesc.InnerRadius = g["InnerRadius"].as<float>();
				if (g["OuterRadius"]) GeneratorDesc.OuterRadius = g["OuterRadius"].as<float>();
				if (g["StepDistance"]) GeneratorDesc.StepDistance = g["StepDistance"].as<float>();
				if (g["WallOffset"]) GeneratorDesc.WallOffset = g["WallOffset"].as<float>();
				if (g["ConeAngle"]) GeneratorDesc.ConeAngle = g["ConeAngle"].as<float>();
				if (g["IncludeGrid"]) GeneratorDesc.IncludeGrid = g["IncludeGrid"].as<bool>();
				if (g["ProjectToTerrain"]) GeneratorDesc.ProjectToTerrain = g["ProjectToTerrain"].as<bool>();
			}

			Tests.clear();
			if (root["Tests"] && root["Tests"].IsSequence())
			{
				for (const auto& tNode : root["Tests"])
				{
					EQSTestDesc t;
					if (tNode["Name"]) t.Name = tNode["Name"].as<std::string>();
					if (tNode["Type"]) t.TestType = tNode["Type"].as<std::string>();
					if (tNode["Context"]) t.ContextType = tNode["Context"].as<std::string>();
					if (tNode["Purpose"]) t.Purpose = StringToPurpose(tNode["Purpose"].as<std::string>());
					if (tNode["Curve"]) t.Curve = StringToCurve(tNode["Curve"].as<std::string>());
					if (tNode["Weight"]) t.Weight = tNode["Weight"].as<float>();
					if (tNode["FilterMin"]) t.FilterMin = tNode["FilterMin"].as<float>();
					if (tNode["FilterMax"]) t.FilterMax = tNode["FilterMax"].as<float>();
					if (tNode["IdealValue"]) t.IdealValue = tNode["IdealValue"].as<float>();
					if (tNode["GaussianSigma"]) t.GaussianSigma = tNode["GaussianSigma"].as<float>();
					if (tNode["ScriptPath"]) t.ScriptPath = tNode["ScriptPath"].as<std::string>();
					if (tNode["Enabled"]) t.Enabled = tNode["Enabled"].as<bool>();
					Tests.push_back(std::move(t));
				}
			}

			return true;
		}
		catch (const std::exception& e)
		{
			if (error) *error = e.what();
			return false;
		}
	}

	bool EQSAsset::SaveToFile(const std::string& physicalPath, std::string* error) const
	{
		try
		{
			std::filesystem::path fsPath(physicalPath);
			if (fsPath.has_parent_path())
			{
				std::error_code ec;
				std::filesystem::create_directories(fsPath.parent_path(), ec);
			}

			std::ofstream ofs(fsPath, std::ios::out | std::ios::trunc);
			if (!ofs.is_open())
			{
				if (error) *error = "Failed to open file for writing: " + physicalPath;
				return false;
			}

			ofs << ToYamlString();
			ofs.close();
			return true;
		}
		catch (const std::exception& e)
		{
			if (error) *error = e.what();
			return false;
		}
	}

	bool EQSAsset::LoadFromFile(const std::string& physicalPath, std::string* error)
	{
		try
		{
			std::ifstream ifs(physicalPath);
			if (!ifs.is_open())
			{
				if (error) *error = "Failed to open file for reading: " + physicalPath;
				return false;
			}

			std::stringstream ss;
			ss << ifs.rdbuf();
			return FromYamlString(ss.str(), error);
		}
		catch (const std::exception& e)
		{
			if (error) *error = e.what();
			return false;
		}
	}

	std::shared_ptr<EQSQuery> EQSAsset::BuildQuery(uint32_t targetHint) const
	{
		// 1. 生成器实例化
		std::shared_ptr<IEQSGenerator> generator;
		if (GeneratorDesc.Type == "Circle")
		{
			generator = std::make_shared<EQSGenerator_Circle>(
				GeneratorDesc.InnerRadius, GeneratorDesc.OuterRadius, GeneratorDesc.StepDistance, GeneratorDesc.ProjectToTerrain);
		}
		else if (GeneratorDesc.Type == "AroundObstacles")
		{
			generator = std::make_shared<EQSGenerator_AroundObstacles>(
				GeneratorDesc.WallOffset, GeneratorDesc.IncludeGrid, GeneratorDesc.ProjectToTerrain);
		}
		else if (GeneratorDesc.Type == "Cone")
		{
			generator = std::make_shared<EQSGenerator_Cone>(
				GeneratorDesc.ConeAngle, GeneratorDesc.Radius, GeneratorDesc.InnerRadius, GeneratorDesc.StepDistance, 15.0f, GeneratorDesc.ProjectToTerrain);
		}
		else
		{
			generator = std::make_shared<EQSGenerator_ProjectedGrid>(
				GeneratorDesc.Radius, GeneratorDesc.Spacing, GeneratorDesc.ProjectToTerrain);
		}

		EQSQuery::Builder builder;
		builder.Named(Name).UsingGenerator(generator);

		// 2. 测试项流水线依次装配
		for (const auto& t : Tests)
		{
			if (!t.Enabled)
				continue;

			std::shared_ptr<IEQSTest> test;
			if (t.TestType == "Distance")
			{
				test = std::make_shared<EQSTest_Distance>();
			}
			else if (t.TestType == "Trace" || t.TestType == "TraceLOS")
			{
				auto tr = std::make_shared<EQSTest_Trace>();
				tr->InvertOcclusion = (t.Curve == ScoringCurve::Inverse);
				test = tr;
			}
			else if (t.TestType == "Dot" || t.TestType == "DotAngle")
			{
				test = std::make_shared<EQSTest_Dot>();
			}
			else if (t.TestType == "WallProximity")
			{
				test = std::make_shared<EQSTest_WallProximity>();
			}
			else if (t.TestType == "Height")
			{
				test = std::make_shared<EQSTest_Height>();
			}
			else if (t.TestType == "Overlap")
			{
				test = std::make_shared<EQSTest_Overlap>();
			}
			else if (t.TestType == "FriendlyPenalty")
			{
				test = std::make_shared<EQSTest_FriendlyPenalty>();
			}
			else if (t.TestType == "CoverReservation")
			{
				test = std::make_shared<EQSTest_CoverReservation>();
			}
			else if (t.TestType == "Pathfinding")
			{
				test = std::make_shared<EQSTest_Pathfinding>();
			}
			else if (t.TestType == "FieldOfView" || t.TestType == "FOV")
			{
				test = std::make_shared<EQSTest_FieldOfView>();
			}
			else if (t.TestType == "Scripted")
			{
				test = std::make_shared<EQSTest_Scripted>(t.ScriptPath);
			}
			else
			{
				test = EQSTestRegistry::Get().Find(t.TestType);
			}

			if (test)
			{
				test->Purpose = t.Purpose;
				test->Curve = t.Curve;
				test->Weight = t.Weight;
				test->FilterMin = t.FilterMin;
				test->FilterMax = t.FilterMax;
				test->IdealValue = t.IdealValue;
				test->GaussianSigma = t.GaussianSigma;

				if (t.ContextType == "Querier")
				{
					test->Context = std::make_shared<EQSContext_Querier>();
				}
				else if (t.ContextType == "AllThreats")
				{
					test->Context = std::make_shared<EQSContext_AllThreats>();
				}
				else if (t.ContextType == "AllAllies")
				{
					test->Context = std::make_shared<EQSContext_AllAllies>();
				}
				else if (t.ContextType == "CustomLocation")
				{
					test->Context = std::make_shared<EQSContext_CustomLocation>();
				}
				else
				{
					test->Context = std::make_shared<EQSContext_Target>(targetHint);
				}

				builder.AddTest(test);
			}
		}

		return builder.Build();
	}

	std::shared_ptr<EQSAsset> EQSAsset::GetOrLoad(const std::string& logicalOrPhysicalPath)
	{
		if (logicalOrPhysicalPath.empty())
			return nullptr;

		auto it = s_AssetCache.find(logicalOrPhysicalPath);
		if (it != s_AssetCache.end() && it->second)
		{
			return it->second;
		}

		std::filesystem::path cand = logicalOrPhysicalPath;
		if (!std::filesystem::exists(cand))
		{
			cand = Paths::ProjectFile(logicalOrPhysicalPath);
		}
		if (!std::filesystem::exists(cand))
		{
			cand = Paths::AssetRoot() / logicalOrPhysicalPath;
		}

		if (std::filesystem::exists(cand))
		{
			auto asset = std::make_shared<EQSAsset>();
			if (asset->LoadFromFile(cand.string()))
			{
				s_AssetCache[logicalOrPhysicalPath] = asset;
				return asset;
			}
		}

		return nullptr;
	}

	void EQSAsset::ClearCache()
	{
		s_AssetCache.clear();
	}
}
