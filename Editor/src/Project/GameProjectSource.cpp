#include "wldpch.h"
#include "GameProjectSource.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace World::Editor
{
	namespace
	{
		// 注释 / 字符串字面量与真代码的区分:所有"是否已登记"的判定只在 Code 状态里命中。
		// 背景(templates/project-empty/src/GameProject.cpp):两个函数体里各有**示例注释** ——
		//     // scene.RegisterSystem<MySystem>();
		//     // scene.UnregisterFrameSystem("MySystem");
		// 天真的子串查找会把它们当成"已登记",于是用户新建名为 MySystem 的系统时一行都不插、
		// 还回报 AlreadyRegistered(静默失效)。所以判定与插入都必须走这里。
		enum class CodeContext : unsigned char
		{
			Code = 0,
			Comment,
			String,
		};

		std::vector<CodeContext> ComputeCodeContexts(const std::string& text)
		{
			std::vector<CodeContext> contexts(text.size(), CodeContext::Code);
			bool inLineComment = false;
			bool inBlockComment = false;
			char quote = 0;
			for (size_t i = 0; i < text.size(); ++i)
			{
				const char c = text[i];
				const char next = (i + 1 < text.size()) ? text[i + 1] : '\0';
				if (inLineComment)
				{
					contexts[i] = CodeContext::Comment;
					if (c == '\n')
						inLineComment = false;
					continue;
				}
				if (inBlockComment)
				{
					contexts[i] = CodeContext::Comment;
					if (c == '*' && next == '/')
					{
						contexts[i + 1] = CodeContext::Comment;
						inBlockComment = false;
						++i;
					}
					continue;
				}
				if (quote)
				{
					contexts[i] = CodeContext::String;
					if (c == '\\')
					{
						if (i + 1 < text.size())
							contexts[i + 1] = CodeContext::String;
						++i;
						continue;
					}
					if (c == quote)
						quote = 0;
					continue;
				}
				if (c == '/' && next == '/')
				{
					contexts[i] = CodeContext::Comment;
					inLineComment = true;
					continue;
				}
				if (c == '/' && next == '*')
				{
					contexts[i] = CodeContext::Comment;
					inBlockComment = true;
					continue;
				}
				if (c == '"' || c == '\'')
				{
					contexts[i] = CodeContext::String;
					quote = c;
					continue;
				}
				contexts[i] = CodeContext::Code;
			}
			return contexts;
		}

		bool IsCodeAt(const std::vector<CodeContext>& contexts, size_t index)
		{
			return index < contexts.size() && contexts[index] == CodeContext::Code;
		}

		size_t FindInCode(const std::string& text, const std::vector<CodeContext>& contexts,
			const std::string& needle, size_t from = 0)
		{
			size_t at = text.find(needle, from);
			while (at != std::string::npos)
			{
				if (IsCodeAt(contexts, at))
					return at;
				at = text.find(needle, at + 1);
			}
			return std::string::npos;
		}

		// **只在 [from, to) 的真代码里**找 needle:注释 / 字符串字面量里的同名字样不算。
		bool ContainsInCode(const std::string& text, const std::vector<CodeContext>& contexts,
			size_t from, size_t to, const std::string& needle)
		{
			if (needle.empty() || from >= to)
				return false;
			size_t at = text.find(needle, from);
			while (at != std::string::npos && at + needle.size() <= to)
			{
				if (IsCodeAt(contexts, at))
					return true;
				at = text.find(needle, at + 1);
			}
			return false;
		}

		size_t FindBodyOpenBrace(const std::string& text, const std::vector<CodeContext>& contexts, size_t from)
		{
			for (size_t i = from; i < text.size(); ++i)
				if (text[i] == '{' && IsCodeAt(contexts, i))
					return i;
			return std::string::npos;
		}

		// 函数体 `{` 的配对 `}` 下标(跳过字符串字面量与注释里的花括号);找不到/不配对返回 npos。
		size_t FindBodyCloseBrace(const std::string& text, const std::vector<CodeContext>& contexts, size_t from)
		{
			const size_t open = FindBodyOpenBrace(text, contexts, from);
			if (open == std::string::npos)
				return std::string::npos;
			int depth = 0;
			for (size_t i = open; i < text.size(); ++i)
			{
				if (!IsCodeAt(contexts, i))
					continue;
				if (text[i] == '{')
					++depth;
				else if (text[i] == '}')
				{
					if (--depth == 0)
						return i;
				}
			}
			return std::string::npos;
		}

		// 缩进:插进函数体的那一行用函数体当前的缩进 + 一层(本工程的 GameProject.cpp 惯例是 tab)。
		std::string IndentOfBody(const std::string& text, size_t functionStart, size_t bodyClose)
		{
			size_t lineStart = bodyClose;
			while (lineStart > functionStart && text[lineStart - 1] != '\n')
				--lineStart;
			std::string indent;
			for (size_t i = lineStart; i < bodyClose && (text[i] == '\t' || text[i] == ' '); ++i)
				indent.push_back(text[i]);
			return indent + "\t";
		}

		// 在 `bodyClose` 之前插入 `line`(自带换行),保持文件其余部分逐字节不变。
		void InsertBefore(std::string& text, size_t bodyClose, const std::string& indent, const std::string& line)
		{
			size_t lineStart = bodyClose;
			while (lineStart > 0 && text[lineStart - 1] != '\n')
				--lineStart;
			const std::string original = text.substr(lineStart, bodyClose - lineStart);
			if (original.find_first_not_of(" \t") != std::string::npos)
			{
				// `}` 与别的语句同行:不动它的行,直接在 `}` 前插(极少见,但别猜)。
				text.insert(bodyClose, indent + line + "\n");
				return;
			}
			text.replace(lineStart, bodyClose - lineStart, indent + line + "\n" + original);
		}

		// 在顶部 `#include "GameAPI.h"` 那一行之后插 `#include "Systems/<Name>.h"`(沿用锚点行的缩进)。
		// 找不到锚点 ⇒ false(调用方**整份不写**)。已有同一条 include 时调用方不会再调这里。
		bool InsertSystemsInclude(std::string& text, const std::vector<CodeContext>& contexts,
			const std::string& systemName)
		{
			const std::string anchor = "#include \"GameAPI.h\"";
			size_t anchorAt = std::string::npos;
			size_t at = FindInCode(text, contexts, anchor);
			while (at != std::string::npos)
			{
				const size_t lineStart = text.rfind('\n', at);
				const size_t begin = (lineStart == std::string::npos) ? 0 : lineStart + 1;
				bool onlyWhitespace = true;
				for (size_t i = begin; i < at; ++i)
					if (text[i] != ' ' && text[i] != '\t')
						onlyWhitespace = false;
				if (onlyWhitespace)
				{
					anchorAt = at;
					break;
				}
				at = FindInCode(text, contexts, anchor, at + 1);
			}
			if (anchorAt == std::string::npos)
				return false;

			const size_t lineStart = text.rfind('\n', anchorAt);
			const size_t begin = (lineStart == std::string::npos) ? 0 : lineStart + 1;
			std::string indent;
			for (size_t i = begin; i < anchorAt; ++i)
				indent.push_back(text[i]);
			const std::string line = indent + "#include \"Systems/" + systemName + ".h\"";
			const size_t lineEnd = text.find('\n', anchorAt);
			if (lineEnd == std::string::npos)
			{
				text += "\n" + line;
				return true;
			}
			text.insert(lineEnd + 1, line + "\n");
			return true;
		}

		// 临时文件 + 改名。失败不留半成品,**也不丢旧文件**:直接改名(MSVC 允许覆盖目标)失败时,
		// 先把旧文件改名成 `.bak-edit`、让新文件就位、再删备份;中途失败把旧文件放回去。
		bool WriteFileAtomically(const std::filesystem::path& path, const std::string& contents,
			std::string* error)
		{
			const std::filesystem::path temporary = path.string() + ".tmp-edit";
			std::error_code directoryError;
			std::filesystem::create_directories(path.parent_path(), directoryError);
			if (directoryError)
			{
				if (error) *error = "cannot create " + path.parent_path().string()
					+ " (" + directoryError.message() + ")";
				return false;
			}
			{
				std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
				if (!out.is_open())
				{
					if (error) *error = "cannot write " + temporary.string();
					return false;
				}
				out << contents;
				out.flush();
				if (!out.good())
				{
					if (error) *error = "write failed: " + temporary.string();
					return false;
				}
			}

			std::error_code renameError;
			std::filesystem::rename(temporary, path, renameError);
			if (!renameError)
				return true;

			// 目标已存在且实现拒绝覆盖:走"备份旧文件 → 新文件就位 → 删备份"。
			const std::filesystem::path backup = path.string() + ".bak-edit";
			std::error_code backupError;
			std::filesystem::rename(path, backup, backupError);
			if (!backupError)
			{
				std::error_code secondError;
				std::filesystem::rename(temporary, path, secondError);
				if (!secondError)
				{
					std::error_code cleanupBackup;
					std::filesystem::remove(backup, cleanupBackup);
					return true;
				}
				std::error_code restoreError;
				std::filesystem::rename(backup, path, restoreError);
				std::error_code cleanup;
				std::filesystem::remove(temporary, cleanup);
				if (error) *error = "cannot move the edit into place (" + secondError.message() + ")";
				return false;
			}

			std::error_code cleanup;
			std::filesystem::remove(temporary, cleanup);
			if (error) *error = "cannot replace " + path.string() + " (" + renameError.message() + ")";
			return false;
		}
	}

	bool IsValidSystemIdentifier(const std::string& name)
	{
		if (name.empty())
			return false;
		const auto letter = [](unsigned char c)
		{
			return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
		};
		const unsigned char first = static_cast<unsigned char>(name[0]);
		if (!letter(first) && first != '_')
			return false;
		for (const char raw : name)
		{
			const unsigned char c = static_cast<unsigned char>(raw);
			if (!letter(c) && !(c >= '0' && c <= '9') && c != '_')
				return false;
		}
		return true;
	}

	std::string GameProjectSkeletonSource()
	{
		return
			"#include \"GameAPI.h\"\n"
			"\n"
			"// 项目层系统挂载入口。空模板里一个项目系统都没有,所以函数体是空的 —— 这就是\"从零开始\"\n"
			"// 的起点:\n"
			"//   1) 在 src/Systems/ 写一个 World::ISystem(一页教程见 src/Systems/README.md);\n"
			"//   2) 在这里 #include \"Systems/MySystem.h\",Attach 里 scene.RegisterSystem<MySystem>(),\n"
			"//      Detach 里 scene.UnregisterFrameSystem(\"MySystem\")(字符串必须与 Name() 逐字相同);\n"
			"//   3) 项目根构建:build.cmd → 编辑器 `文件 ▸ 重载 C++ 模块` → Play。\n"
			"//\n"
			"// 引擎在 Scene::OnRuntimeStart 末尾调 Attach、OnRuntimeStop 开头调 Detach:系统只活在一次\n"
			"// 运行时内(Play 停止 → Detach,再 Play → Attach)。\n"
			"namespace World::Game\n"
			"{\n"
			"	void AttachProjectSystems(Scene& scene)\n"
			"	{\n"
			"		(void)scene;\n"
			"		// 目前没有项目系统。示例:\n"
			"		// scene.RegisterSystem<MySystem>();\n"
			"	}\n"
			"\n"
			"	void DetachProjectSystems(Scene& scene)\n"
			"	{\n"
			"		(void)scene;\n"
			"		// 与 Attach 一一对应。示例:\n"
			"		// scene.UnregisterFrameSystem(\"MySystem\");\n"
			"	}\n"
			"}\n";
	}

	std::string GameProjectSystemHeaderSource(const std::string& systemName)
	{
		return
			"#pragma once\n"
			"\n"
			"#include \"World/Core/Timestep.h\"\n"
			"#include \"World/Scene/Components.h\"\n"
			"#include \"World/Scene/ISystem.h\"\n"
			"#include \"World/Scene/Query.h\"\n"
			"#include \"World/Scene/Scene.h\"\n"
			"#include \"World/Scene/TransformSystem.h\"\n"
			"\n"
			"#include <string_view>\n"
			"\n"
			"namespace World::Game\n"
			"{\n"
			"\t// " + systemName + " —— 由编辑器「文件 ▶ 新建 C++ …(系统)」生成。\n"
			"\t//\n"
			"\t// 系统 = 逻辑。实例只在一次运行时内存在:Play/Simulate 开始时由\n"
			"\t// src/GameProject.cpp 的 AttachProjectSystems 挂上,停止时由 DetachProjectSystems 摘掉,\n"
			"\t// 所以不要在这里存跨场景/跨运行的长期状态。\n"
			"\t//\n"
			"\t// 下面这段是范例:查询同时带 Transform + Velocity 的实体并按帧推进位置。\n"
			"\t// 不需要就整段替换(Phase()/ParallelSafe() 的取值按你的系统语义改)。\n"
			"\tclass " + systemName + " : public ISystem\n"
			"\t{\n"
			"\tpublic:\n"
			"\t\tstd::string_view Name() const override { return \"" + systemName + "\"; }\n"
			"\t\tGameplay::SystemPhase Phase() const override { return Gameplay::SystemPhase::Update; }\n"
			"\t\t// true = 只读写自己独占的数据,可与其它并行系统同时跑;碰共享状态就保持 false。\n"
			"\t\tbool ParallelSafe() const override { return false; }\n"
			"\n"
			"\t\tvoid Update(Scene& scene, Timestep dt) override\n"
			"\t\t{\n"
			"\t\t\tconst float delta = dt.GetSeconds();\n"
			"\t\t\tscene.Query<TransformComponent, const VelocityComponent>()\n"
			"\t\t\t\t.Each([delta](TransformComponent& transform, const VelocityComponent& velocity) {\n"
			"\t\t\t\t\ttransform.Location += velocity.Linear * delta;\n"
			"\t\t\t\t\tTransformSystem::Recalculate(transform);\n"
			"\t\t\t\t});\n"
			"\t\t}\n"
			"\t};\n"
			"}\n";
	}

	GameProjectSystemRegistration RegisterProjectSystem(const std::filesystem::path& projectRoot,
		const std::string& systemName)
	{
		GameProjectSystemRegistration result;
		if (!IsValidSystemIdentifier(systemName))
		{
			result.Error = "system name must be a valid C++ identifier: " + systemName;
			return result;
		}

		const std::filesystem::path path = projectRoot / "src" / "GameProject.cpp";
		result.Path = path.u8string();

		std::string text;
		{
			std::ifstream in(path, std::ios::binary);
			if (in.is_open())
			{
				std::ostringstream buffer;
				buffer << in.rdbuf();
				text = buffer.str();
			}
		}
		if (text.empty())
		{
			text = GameProjectSkeletonSource();
			result.FileCreated = true;
		}

		// 三处登记点(与模板/文档里的写法逐字一致)。
		const std::string includeLine = "#include \"Systems/" + systemName + ".h\"";
		const std::string attachLine = "scene.RegisterSystem<" + systemName + ">();";
		const std::string detachLine = "scene.UnregisterFrameSystem(\"" + systemName + "\");";

		const std::vector<CodeContext> contexts = ComputeCodeContexts(text);
		const std::string attachMarker = "void AttachProjectSystems(";
		const std::string detachMarker = "void DetachProjectSystems(";
		const size_t attachAt = FindInCode(text, contexts, attachMarker);
		const size_t detachAt = FindInCode(text, contexts, detachMarker);
		if (attachAt == std::string::npos || detachAt == std::string::npos)
		{
			result.Error = "GameProject.cpp does not declare AttachProjectSystems / "
				"DetachProjectSystems; add the system manually (the file was not modified)";
			return result;
		}

		const size_t attachOpen = FindBodyOpenBrace(text, contexts, attachAt);
		const size_t attachClose = FindBodyCloseBrace(text, contexts, attachAt);
		if (attachOpen == std::string::npos || attachClose == std::string::npos)
		{
			result.Error = "cannot find the body of AttachProjectSystems (the file was not modified)";
			return result;
		}
		const size_t detachOpen = FindBodyOpenBrace(text, contexts, detachAt);
		const size_t detachClose = FindBodyCloseBrace(text, contexts, detachAt);
		if (detachOpen == std::string::npos || detachClose == std::string::npos)
		{
			result.Error = "cannot find the body of DetachProjectSystems (the file was not modified)";
			return result;
		}

		// **只在真代码里**判定(模板里的示例注释不算已登记)。
		const bool includePresent = ContainsInCode(text, contexts, 0, text.size(), includeLine);
		const bool attachPresent = ContainsInCode(text, contexts, attachOpen, attachClose + 1, attachLine);
		const bool detachPresent = ContainsInCode(text, contexts, detachOpen, detachClose + 1, detachLine);
		if (attachPresent && detachPresent)
		{
			result.Ok = true;
			result.AlreadyRegistered = true;
			return result;
		}

		// 按**位置从后往前**插,两个函数谁先谁后都不会错位。
		struct PendingInsertion
		{
			size_t Close = 0;
			std::string Indent;
			std::string Line;
		};
		std::vector<PendingInsertion> insertions;
		if (!detachPresent)
			insertions.push_back({ detachClose, IndentOfBody(text, detachAt, detachClose), detachLine });
		if (!attachPresent)
			insertions.push_back({ attachClose, IndentOfBody(text, attachAt, attachClose), attachLine });
		std::sort(insertions.begin(), insertions.end(),
			[](const PendingInsertion& a, const PendingInsertion& b) { return a.Close > b.Close; });
		for (const PendingInsertion& insertion : insertions)
			InsertBefore(text, insertion.Close, insertion.Indent, insertion.Line);
		result.AttachAdded = !attachPresent;
		result.DetachAdded = !detachPresent;

		// include 通常在最上面,但**不假定**:函数体插入完成后按**当前文本**重算一遍上下文与锚点,
		// 再补这一行(避免"include 写在函数体之后"这种合法但少见的布局把下标算错)。
		// 已经登记过一半的既有文件不补 include —— attach 行能编译说明系统定义本来就可见(include
		// 可能写在别的头里,或是 `#include <Systems/X.h>` 这类写法),这时再插一条
		// `#include "Systems/<Name>.h"` 反而可能把同名类重复引入(示例模板的 ExampleSystems.h 就是
		// 一个文件里定义多个系统)。
		if (!includePresent && !attachPresent && !detachPresent)
		{
			const std::vector<CodeContext> includeContexts = ComputeCodeContexts(text);
			if (!InsertSystemsInclude(text, includeContexts, systemName))
			{
				result.Error = "GameProject.cpp has no '#include \"GameAPI.h\"' line to anchor the system "
					"include; add '#include \"Systems/" + systemName + ".h\"' plus the two registration "
					"lines manually (the file was not modified)";
				return result;
			}
			result.IncludeAdded = true;
		}

		std::string error;
		if (!WriteFileAtomically(path, text, &error))
		{
			result.Error = error;
			return result;
		}
		result.Ok = true;
		return result;
	}
}
