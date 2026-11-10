#pragma once

#include "World/Core/Export.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace World::Ai
{
	struct WLD_API ValidationError
	{
		std::string code;
		std::string file;
		int line = 0;
		std::string message;
	};

	struct WLD_API ValidationReport
	{
		bool valid = true;
		std::vector<ValidationError> errors;
		std::vector<std::string> warnings;
		std::map<std::string, std::string> metrics;

		std::string ToJson() const;
	};

	class WLD_API SelfValidation
	{
	public:
		// 校验项目根（清单 project.we.yaml、assets/ 目录规范、全量脚本语法编译、引擎树指纹、未归属变动）
		static ValidationReport ValidateProject(const std::string& projectRoot = "");

		// 校验指定资产（存在性、真实格式头签名、依赖拓扑闭包、2D/3D 美术规范预算与维度约束）
		static ValidationReport ValidateAsset(const std::string& assetPath, const std::string& projectRoot = "");

		// 导出当前注册的全量组件列表 JSON: {"components": [...]}
		static std::string ListSchemas();

		// 导出指定组件的字段 Schema 详细反射元数据 JSON
		static std::string DescribeSchema(const std::string& componentName);

		// 导出 Luau 运行时 API 存根 (WorldEngineAPI.luau) 内容
		static std::string GetLuauStubs(const std::string& projectRoot = "");

		// 搜索引擎文档、项目文档与角色守则 (docs.search)
		static std::string SearchDocs(const std::string& query, const std::string& projectRoot = "");

		// 编译 CSV/YAML 配置表为类型化数据资产 (datatable.compile)
		static std::string CompileDataTable(const std::string& tablePath, const std::string& projectRoot = "");

		// 统一资产导入并支持持久 AssetId 原地平滑替换 (asset.import)
		static std::string ImportAsset(const std::string& sourcePath, const std::string& targetRelPath = "", const std::string& replaceAssetId = "", const std::string& projectRoot = "");

		// 注册 project.validate, asset.validate, schema.list, schema.describe, api.luau_stubs, docs.search, datatable.compile, asset.import 到 CommandRegistry
		static void RegisterCommands();
		static void RegisterCommands(class CommandRegistry& reg);
	};
}
