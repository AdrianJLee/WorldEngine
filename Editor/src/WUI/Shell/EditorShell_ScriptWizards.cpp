#include "EditorShell_Internal.h"
#include "World/Core/StringPool.h"

namespace World
{

using namespace EditorShellDetail;

namespace EditorShellDetail
{
		// 名称 → 文件名:去掉用户可能顺手输入的 `.h` 后缀与首尾空白(与内容浏览器同名口径)。
std::string CppScriptBaseName(const std::string& text){
			std::string name = text;
			while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
				name.erase(name.begin());
			while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
				name.pop_back();
			if (name.size() > 2 && name.compare(name.size() - 2, 2, ".h") == 0)
				name.erase(name.size() - 2);
			return name;
		}


		// 合法 C++ 标识符:[A-Za-z_][A-Za-z0-9_]*。
bool IsValidCppIdentifier(const std::string& name){
			if (name.empty())
				return false;
			const auto letter = [](unsigned char c)
			{
				return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
			};
			if (!letter(static_cast<unsigned char>(name[0])) && name[0] != '_')
				return false;
			for (const char character : name)
			{
				const unsigned char c = static_cast<unsigned char>(character);
				if (!letter(c) && !(c >= '0' && c <= '9') && c != '_')
					return false;
			}
			return true;
		}


		// PECS-T11:下拉的**显示名**走本地化(中文界面画「组件 / 系统 / 空」),无障碍 value 仍是
		// 上面的英文术语 —— 画完 Combo 后按同一 id 再登记一次覆盖回术语(后写覆盖,与内容浏览器
		// 菜单项同一手法),脚本/探针的断言口径不变。
const char* CppScriptKindTerm(int kind){
			switch (kind)
			{
				case kCppScriptKindSystem: return kCppScriptKindSystemTerm;
				case kCppScriptKindEmpty: return kCppScriptKindEmptyTerm;
				default: return kCppScriptKindComponentTerm;
			}
		}


const char* CppScriptKindLabelKey(int kind){
			switch (kind)
			{
				case kCppScriptKindSystem: return "modal.newscript.kind.system";
				case kCppScriptKindEmpty: return "modal.newscript.kind.empty";
				default: return "modal.newscript.kind.component";
			}
		}


		// 相对**项目根**的落点:回显 / 行内错误 / 操作日志统一走这一份口径。
		//   组件 → src/Components/<Name>.h;系统 → src/Systems/<Name>.h;空 → src/<Name>.h。
std::string CppScriptRelativePath(int kind, const std::string& name){
			switch (kind)
			{
				case kCppScriptKindSystem: return "src/Systems/" + name + ".h";
				case kCppScriptKindEmpty: return "src/" + name + ".h";
				default: return "src/Components/" + name + ".h";
			}
		}


		// 操作日志里的目录口径(默认类型的值与既有 `new-cpp-ask` 逐字相同)。
std::string CppScriptRelativeDir(int kind){
			switch (kind)
			{
				case kCppScriptKindSystem: return "src/Systems";
				case kCppScriptKindEmpty: return "src";
				default: return "src/Components";
			}
		}


		// ---- PECS-T9:File ▸ New Lua System… ----

		// 系统名 → 文件名:剥掉 `.luau` / `.lua` 后缀与首尾空白(与 CppScriptBaseName 同口径)。
std::string LuaSystemBaseName(const std::string& text){
			std::string name = text;
			while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
				name.erase(name.begin());
			while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
				name.pop_back();
			for (const char* suffix : { ".luau", ".lua" })
			{
				const size_t length = std::strlen(suffix);
				if (name.size() > length && name.compare(name.size() - length, length, suffix) == 0)
				{
					name.erase(name.size() - length);
					break;
				}
			}
			return name;
		}


		// 资产文件名合法性(Windows 口径,与内容浏览器的资产命名同一套):非空、不是 '.'/'..'、
		// 不含 `\ / : * ? " < > |`、不以点或空格结尾。空串由调用方先给"不能为空"。
bool IsValidLuaScriptFileName(const std::string& name){
			if (name.empty() || name == "." || name == "..")
				return false;
			for (const char character : name)
			{
				if (character == '\\' || character == '/' || character == ':' || character == '*'
					|| character == '?' || character == '"' || character == '<' || character == '>'
					|| character == '|')
					return false;
			}
			return name.back() != '.' && name.back() != ' ';
		}


		// 模板缺失时的内置骨架 = 磁盘模板的系统脚本形态,签名用**规范顺序**
		// `ecs:AddSystem(名字, 函数, 阶段)`(旧顺序仍兼容,但新生成的骨架不该示范旧写法)。
std::string NewLuaSystemTemplateSource(){
			std::string source;
			source += "-- 新建系统脚本模板。\n";
			source += "-- 放到 <内容根>/scripts/systems/ 下才会被自动加载 —— 场景启动时整份执行一次,\n";
			source += "-- 之后每帧按你注册的阶段调用。\n";
			source += "-- 提示来自编辑器生成的 assets/scripts/intermediate/WorldEngineAPI.luau。\n\n";
			source += "-- 支持使用 Comp 常量表避免手写字符串:Comp.Transform 等价于 \"TransformComponent\"\n";
			source += "local query = ecs:Query({ Comp.Transform, Comp.Velocity })\n\n";
			source += "-- 规范签名是 (名字, 函数 [, 阶段]);阶段可直接使用 Phase 枚举(如 Phase.Update)。\n";
			source += "-- 也支持单表配置形态: ecs:AddSystem({ name = \"NewSystem\", update = function(dt: number) ... end, phase = Phase.Update })\n";
			source += "ecs:AddSystem(\"NewSystem\", function(dt: number)\n";
			source += "    query:Each(function(entity: Entity, transform: any, velocity: any)\n";
			source += "        transform.Location.x = transform.Location.x + velocity.Linear.x * dt\n";
			source += "        transform.Location.y = transform.Location.y + velocity.Linear.y * dt\n";
			source += "        transform.Location.z = transform.Location.z + velocity.Linear.z * dt\n";
			source += "    end)\n";
			source += "end, Phase.Update)\n\n";
			source += "-- 需要\"某个组件出现/消失时做一次事\",用观察者代替轮询:\n";
			source += "-- ecs:OnAdd(Comp.Tag, function(entity: Entity) print(\"tag added\", entity:GetName()) end)\n";
			return source;
		}


		// 与 C++ 向导同一条口径:下拉显示本地化名字,a11y value 保持英文术语(画完覆盖登记)。
const char* LuaScriptKindTerm(int kind){
			switch (kind)
			{
				case kLuaScriptKindLibrary: return kLuaScriptKindLibraryTerm;
				case kLuaScriptKindEmpty: return kLuaScriptKindEmptyTerm;
				default: return kLuaScriptKindSystemTerm;
			}
		}


const char* LuaScriptKindLabelKey(int kind){
			switch (kind)
			{
				case kLuaScriptKindLibrary: return "modal.newlua.kind.library";
				case kLuaScriptKindEmpty: return "modal.newlua.kind.empty";
				default: return "modal.newlua.kind.system";
			}
		}


std::string LuaScriptRelativePath(int kind, const std::string& name){
			switch (kind)
			{
				case kLuaScriptKindLibrary: return "scripts/lib/" + name + ".luau";
				case kLuaScriptKindEmpty: return "scripts/" + name + ".luau";
				default: return "scripts/systems/" + name + ".luau";
			}
		}


std::string LuaScriptRelativeDir(int kind){
			switch (kind)
			{
				case kLuaScriptKindLibrary: return "scripts/lib";
				case kLuaScriptKindEmpty: return "scripts";
				default: return "scripts/systems";
			}
		}


		// 「脚本库」的内置骨架:`local M = {}` … `return M`。
		// 注释刻意不写死 `require`:当前 Luau 沙箱里 `require` 是 nil(Engine/src/World/Script/Vm/LuauVm.cpp
		// 的 kForbiddenGlobals),库文件的加载通道不在本轮编辑器改动范围内 —— 见任务报告"未决项"。
std::string NewLuaLibraryTemplateSource(){
			std::string source;
			source += "-- Lua 脚本库(scripts/lib/):不会自动加载 —— 只有 scripts/systems/ 下的脚本会在 Play 时被装载。\n";
			source += "-- 这里放可复用的库代码(约定:local M = {} … return M)。\n\n";
			source += "local M = {}\n\n";
			source += "return M\n";
			return source;
		}


		// 「空」的内置骨架:只有一段说明注释,说清"只有 scripts/systems/ 下的会被自动加载"。
std::string NewLuaEmptyTemplateSource(){
			std::string source;
			source += "-- Lua 脚本:放在 <内容根>/scripts/ 下不会自动加载 ——\n";
			source += "-- 只有 scripts/systems/ 下的脚本会在 Play 时被自动执行。\n";
			source += "-- 要让它跑起来:移到 scripts/systems/,或用「文件 ▶ 新建 Lua …」选「系统」。\n";
			return source;
		}


		// 模板正文:头注释(组件 = 纯数据 / 逻辑写在 src/Systems/ / 构建后重载 C++ 模块)+ 标量
		// (Default/Range/Unit/Step/Doc)+ 枚举 + 命名 struct(Object, Of(...))+
		// Array/Map 容器 + `WE_SCHEMA_BODY(Game, <Name>, Component)`。
		// 每个类型名都带组件名前缀(`<Name>Mode` / `<Name>Data`):文件名唯一由校验保证,
		// 生成注册单元同时包含多个组件头时也不会重定义。
std::string NewCppScriptTemplateSource(const std::string& name){
			std::string source;
			source += "#pragma once\n";
			source += "#include \"World/Scene/Components.h\"\n\n";
			source += "#include <map>\n";
			source += "#include <string>\n";
			source += "#include <vector>\n\n";
			source += "namespace World\n";
			source += "{\n";
			source += "\t// ============================================================================\n";
			source += "\t// " + name + " — 由编辑器「文件 ▶ 新建 C++ 组件…」生成的纯 ECS 组件模板。\n";
			source += "\t//\n";
			source += "\t// ① 这是**组件 = 纯数据**:只声明字段与 WE_FIELD 反射元数据,不写逻辑;\n";
			source += "\t//    编辑器属性面板按 WE_FIELD 的声明自动画控件。\n";
			source += "\t// ② 逻辑写在 <项目根>/src/Systems/ 的系统里,并在 <项目根>/src/GameProject.cpp 里挂载:\n";
			source += "\t//    AttachProjectSystems 里 scene.RegisterSystem<...>();\n";
			source += "\t//    DetachProjectSystems 里 scene.UnregisterSystem<...>();\n";
			source += "\t//    系统只活在一次运行时内(Play 启停各一次)。\n";
			source += "\t// ③ 构建项目后回编辑器执行「文件 ▶ 重载 C++ 模块」加载新组件。\n";
			source += "\t//\n";
			source += "\t// 结构与示例项目模板 src/Components/SampleDataComponent.h 一致:标量 / 枚举 / 命名\n";
			source += "\t// struct / Array / Map 各留一行范例,不需要的字段整行删掉即可。只有被编辑过的值才写进\n";
			source += "\t// 场景(.wd),未编辑时用成员初始化里的默认值。\n";
			source += "\t// ============================================================================\n\n";
			source += "\t// 枚举:WE_ENUM_SCHEMA 注册后,`Enum, Of(...)` 在面板里是下拉框,存档写整数。\n";
			source += "\tenum class " + name + "Mode : int32_t\n";
			source += "\t{\n";
			source += "\t\tIdle = 0,\n";
			source += "\t\tActive = 1,\n";
			source += "\t};\n";
			source += "\tWE_ENUM_SCHEMA(Game, " + name + "Mode, Int32)\n";
			source += "\t\tWE_ENUM_VALUE(Idle);\n";
			source += "\t\tWE_ENUM_VALUE(Active);\n";
			source += "\tWE_ENUM_END\n\n";
			source += "\t// 命名 struct:字段模型只声明一次;标量字段与容器元素复用同一份(面板展开成子行)。\n";
			source += "\tstruct " + name + "Data\n";
			source += "\t{\n";
			source += "\t\tfloat Amount = 1.0f;\n";
			source += "\t\tint32_t Count = 0;\n\n";
			source += "\t\tWE_SCHEMA_BODY(Game, " + name + "Data, Struct)\n";
			source += "\t\t\tWE_FIELD(Amount, Float, Range(0.0f, 1000.0f),\n";
			source += "\t\t\t\tDoc(\"Nested struct sample: one editable number.\"));\n";
			source += "\t\t\tWE_FIELD(Count, Int32,\n";
			source += "\t\t\t\tDoc(\"Nested struct sample: one editable integer.\"));\n";
			source += "\t\tWE_SCHEMA_END\n";
			source += "\t};\n\n";
			source += "\t// 纯数据组件:不继承任何基类、不写生命周期方法(旧 ScriptableEntity 已删除)。\n";
			source += "\tstruct " + name + "\n";
			source += "\t{\n";
			source += "\t\t// ---- 标量:Default/Range/Unit/Step/Doc ----\n";
			source += "\t\tfloat Speed = 1.0f;\n";
			source += "\t\t" + name + "Mode Mode = " + name + "Mode::Idle;\n";
			source += "\t\t// ---- 命名 struct:Object, Of(...) ----\n";
			source += "\t\t" + name + "Data Data {};\n";
			source += "\t\t// ---- 容器:std::vector<元素> / std::map<std::string, 元素> ----\n";
			source += "\t\tstd::vector<float> Values {};\n";
			source += "\t\tstd::map<std::string, float> Weights {};\n\n";
			source += "\t\tWE_SCHEMA_BODY(Game, " + name + ", Component)\n";
			source += "\t\t\tWE_SCHEMA_META(Category(\"Project\"),\n";
			source += "\t\t\t\tDoc(\"C++ component template generated from the editor: scalar with edit metadata, enum, nested struct and Array/Map container samples.\"))\n";
			source += "\t\t\tWE_FIELD(Speed, Float, Default(1.0f), Range(0.0f, 100.0f), Unit(\"m/s\"), Step(0.1f),\n";
			source += "\t\t\t\tDoc(\"Scalar sample: Step(0.1) is the drag increment; Unit('m/s') is drawn after the value.\"));\n";
			source += "\t\t\tWE_FIELD(Mode, Enum, Of(" + name + "Mode),\n";
			source += "\t\t\t\tDoc(\"Enum sample: the dropdown stores the integer value in the scene.\"));\n";
			source += "\t\t\tWE_FIELD(Data, Object, Of(" + name + "Data),\n";
			source += "\t\t\t\tDoc(\"Nested struct sample: expandable child rows declared once by " + name + "Data.\"));\n";
			source += "\t\t\tWE_FIELD(Values, Array, Of(Float),\n";
			source += "\t\t\t\tDoc(\"Array<Float> sample: one row per element; '+' appends and '-' removes.\"));\n";
			source += "\t\t\tWE_FIELD(Weights, Map, Of(Float),\n";
			source += "\t\t\t\tDoc(\"Map<Float> sample: string key -> number; '+' asks for the key name first.\"));\n";
			source += "\t\tWE_SCHEMA_END\n";
			source += "\t};\n";
			source += "}\n";
			return source;
		}


		// ---- PECS-T11(任务 B):同名查重(纯文本探测,不引 clang)----
		//
		// 目标:建 `Foo` 时,`<项目根>/src/**` 里若已有 `class Foo` / `struct Foo`,或
		// `GameProject.cpp` 里已登记名为 `Foo` 的系统,不等编译器报错,当场给可读的行内原因。
		// 判据刻意保守(**宁可漏报,不误报**):只有"行内 `class`/`struct` 之前全是空白"才算声明形态 —
		// 注释(`// struct Foo`)、`WE_SCHEMA_BODY(…, Struct)` 这类文本不会命中;注释里的同名允许漏报。

std::string LowerFileExtension(const std::filesystem::path& path){
			std::string extension = path.extension().string();
			for (char& character : extension)
				character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			return extension;
		}


		// 行首(仅允许空白)的 `class <Name>` / `struct <Name>` → 返回声明的类型名;否则空串。
std::string DeclaredTypeInLine(const std::string& line){
			size_t start = 0;
			while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
				++start;
			for (const char* keyword : { "class ", "struct " })
			{
				const size_t length = std::strlen(keyword);
				if (line.compare(start, length, keyword) != 0)
					continue;
				size_t at = start + length;
				while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
					++at;
				const size_t begin = at;
				while (at < line.size())
				{
					const unsigned char character = static_cast<unsigned char>(line[at]);
					if (!((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z')
						|| (character >= '0' && character <= '9') || character == '_'))
						break;
					++at;
				}
				if (at > begin)
					return line.substr(begin, at - begin);
			}
			return {};
		}


		// `<项目根>/src/**`(限 .h/.hpp/.cpp;跳过 Generated/ 与隐藏目录)里所有声明形态的类型名。
std::vector<std::string> CollectProjectDeclaredTypes(const std::filesystem::path& sourceRoot){
			std::vector<std::string> names;
			std::error_code walkError;
			const std::filesystem::directory_options options =
				std::filesystem::directory_options::skip_permission_denied;
			for (std::filesystem::recursive_directory_iterator iterator(sourceRoot, options, walkError), end;
				iterator != end; iterator.increment(walkError))
			{
				if (walkError)
				{
					// 单个条目失败(权限/竞态删除)跳过,不打断整个扫描(与 ScriptsPanel 同口径)。
					walkError.clear();
					continue;
				}
				const std::filesystem::directory_entry& entry = *iterator;
				std::error_code entryError;
				if (entry.is_directory(entryError) && !entryError)
				{
					// src/Generated/** 是构建生成物(schema 注册单元 + 类型账本),不代表用户写的类型。
					const std::string directory = entry.path().filename().string();
					if (directory == "Generated" || (!directory.empty() && directory.front() == '.'))
						iterator.disable_recursion_pending();
					continue;
				}
				if (!entry.is_regular_file(entryError) || entryError)
					continue;
				const std::string extension = LowerFileExtension(entry.path());
				if (extension != ".h" && extension != ".hpp" && extension != ".cpp")
					continue;
				std::ifstream in(entry.path(), std::ios::binary);
				if (!in.is_open())
					continue;
				std::string line;
				while (std::getline(in, line))
				{
					if (!line.empty() && line.back() == '\r')
						line.pop_back();
					std::string declared = DeclaredTypeInLine(line);
					if (!declared.empty())
						names.push_back(std::move(declared));
				}
			}
			return names;
		}


		// `GameProject.cpp` 里是否已登记名为 <Name> 的系统(与 GameProjectSource 写出的两行同口径)。
		// 必须忽略注释行,避免骨架/模板里的示例注释 (如 // scene.RegisterSystem<MySystem>();) 误拦截新系统创建。
bool TextRegistersSystem(const std::string& text, const std::string& name){
			size_t pos = 0;
			while (pos < text.size())
			{
				const size_t nextPos = text.find('\n', pos);
				std::string_view line = (nextPos == std::string::npos)
					? std::string_view(text).substr(pos)
					: std::string_view(text).substr(pos, nextPos - pos);
				pos = (nextPos == std::string::npos) ? text.size() : nextPos + 1;

				while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
					line.remove_prefix(1);

				if (line.rfind("//", 0) == 0 || line.rfind("/*", 0) == 0 || line.rfind("*", 0) == 0)
					continue;

				if (line.find("UnregisterSystem<" + name + ">") != std::string_view::npos
					|| line.find("UnregisterFrameSystem(\"" + name + "\")") != std::string_view::npos
					|| line.find("RegisterSystem<" + name + ">(") != std::string_view::npos)
					return true;
			}
			return false;
		}


std::string NewCppEmptyTemplateSource(const std::string& name){
			std::string source;
			source += "#pragma once\n";
			source += "\n";
			source += "// " + name + " —— 由编辑器「文件 ▶ 新建 C++ …(空)」生成的空白文件。\n";
			source += "// 组件请放 src/Components/(用 WE_SCHEMA_BODY 声明,构建后进属性面板与存档);\n";
			source += "// 系统请放 src/Systems/ 并在 src/GameProject.cpp 里成对登记(Attach 里\n";
			source += "// RegisterSystem、Detach 里 UnregisterFrameSystem,名字逐字相同)。\n";
			return source;
		}

}

std::filesystem::path EditorShell::NewCppScriptTargetPath() const{
		// PROJ-8/T1 + PECS-T8/T11:落点在**当前项目层**(项目根取运行期 World::Paths::ProjectDir(),
		// 与进程 CWD 无关)。引擎里的 `Game/src` 只剩模块骨架,用户的 C++ 组件/系统源码属于项目:
		//   * 组件 → `<项目根>/src/Components/<Name>.h`(项目 CMake 的 schema 发现 glob 恰好是
		//     `<项目根>/src/Components/*.h`,随项目构建进 Game.dll);
		//   * 系统 → `<项目根>/src/Systems/<Name>.h`(header-only,由 GameProject.cpp include);
		//   * 空   → `<项目根>/src/<Name>.h`(空白头文件;不参与 schema 发现、不自动登记)。
		// 没有当前项目时 CurrentProjectRoot() 为空 → 调用方先给可读提示,不会走到写盘。
		switch (m_NewCppScriptKind)
		{
			case kCppScriptKindSystem:
				return CurrentProjectRoot() / "src" / "Systems"
					/ (CppScriptBaseName(m_NewCppScriptName) + ".h");
			case kCppScriptKindEmpty:
				return CurrentProjectRoot() / "src" / (CppScriptBaseName(m_NewCppScriptName) + ".h");
			default:
				return CurrentProjectRoot() / "src" / "Components"
					/ (CppScriptBaseName(m_NewCppScriptName) + ".h");
		}
	}


bool EditorShell::NewCppScriptIsSystem() const{
		return m_NewCppScriptKind == kCppScriptKindSystem;
	}


	// PECS-T11(任务 B):"同名类型 / 已登记系统"探测的 0.5s 节流缓存 —— 缓存的是两份**磁盘事实**
	// (与名字无关):`<项目根>/src/**` 的 class/struct 名 + `GameProject.cpp` 正文。每帧调用无负担。
void EditorShell::RefreshCppNameProbe() const{
		const std::filesystem::path root = CurrentProjectRoot();
		const double now = ShellNowSeconds();
		if (m_CppProbeRoot == root.string() && now < m_CppProbeNextScan)
			return;
		m_CppProbeRoot = root.string();
		m_CppProbeNextScan = now + 0.5;
		m_CppProbeTypes.clear();
		m_CppProbeGameProject.clear();
		if (root.empty())
			return;
		m_CppProbeTypes = CollectProjectDeclaredTypes(root / "src");
		const std::filesystem::path gameProject = root / "src" / "GameProject.cpp";
		std::error_code fileError;
		if (!std::filesystem::is_regular_file(gameProject, fileError))
			return;
		std::ifstream in(gameProject, std::ios::binary);
		std::string line;
		while (std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			m_CppProbeGameProject += line;
			m_CppProbeGameProject.push_back('\n');
		}
	}


std::string EditorShell::NewCppScriptNameError() const{
		// 没有当前项目(启动器 / 未打开项目):先给"先打开/新建项目"这条可读原因,
		// 不再去拼一个相对路径(那会按 CWD 误判"已存在")。
		if (CurrentProjectRoot().empty())
			return Wui::Tr("modal.newscript.no_project",
				"No project is open — open or create a project in the launcher first.");
		const std::string name = CppScriptBaseName(m_NewCppScriptName);
		if (name.empty())
			return Wui::Tr("modal.newscript.name.empty", "Name cannot be empty");
		if (!IsValidCppIdentifier(name))
			return Wui::Tr("modal.newscript.name.invalid",
				"Name must be a valid C++ identifier: start with a letter or '_' and use only "
				"letters, digits and '_'");
		std::error_code existsError;
		if (std::filesystem::exists(NewCppScriptTargetPath(), existsError))
		{
			// 落点回显 = 相对**项目根**(与项目源码视图的行标签同一口径)。
			const std::string relative = CppScriptRelativePath(m_NewCppScriptKind, name);
			return Wui::TrFormat("modal.newscript.name.exists",
				"A file with this name already exists: {path}", { { "path", relative } });
		}
		// ---- PECS-T11(任务 B):同名"登记 / 类型"查重(纯文本探测;只在这种明显的形态上命中)----
		RefreshCppNameProbe();
		if (NewCppScriptIsSystem() && TextRegistersSystem(m_CppProbeGameProject, name))
			return Wui::TrFormat("modal.newscript.name.system_exists",
				"A system named {name} is already registered in GameProject.cpp — pick another name.",
				{ { "name", name } });
		for (const std::string& declared : m_CppProbeTypes)
		{
			if (declared == name)
				return Wui::TrFormat("modal.newscript.name.type_exists",
					"A type named {name} already exists under <project>/src — pick another name.",
					{ { "name", name } });
		}
		return {};
	}


void EditorShell::OpenNewCppScriptModal(Wui::WuiContext& ctx){
		// PROJ-8/T1:没有当前项目时不打开一个写不了盘的模态 —— 给一条可读提示,并把这次
		// 意图记进操作日志(与真实菜单动作同口径,方便自动化断言"点过但被拦下")。
		if (CurrentProjectRoot().empty())
		{
			const std::string notice = Wui::Tr("notice.newscript.no_project",
				"No project is open — open or create a project in the launcher first, "
				"then create C++ components.");
			PushNotice(notice);
			ctx.RecordOp("script", "new-cpp-no-project", m_NewCppScriptName, "");
			WLD_CORE_WARN("[new-cpp-script] rejected: no current project");
			return;
		}
		m_NewCppScriptOpen = true;
		m_NewCppScriptOpenedFrame = static_cast<uint32_t>(ctx.Frame());
		// PECS-T8:每次打开都回到默认类型 = 组件(既有自动化"点开向导 → 输名字 → Create"行为不变)。
		m_NewCppScriptKind = kCppScriptKindComponent;
		m_NewCppScriptName = "MyComponent";
		m_NewCppScriptFailure.clear();
		m_NewCppScriptFailureFor.clear();
		ctx.SetModal(Wui::HashId("modal.newscript"));
		ctx.SetFocus(Wui::HashId("script.new.name"));
		ctx.RecordOp("script", "new-cpp-ask", m_NewCppScriptName, CppScriptRelativeDir(m_NewCppScriptKind));
	}


bool EditorShell::CreateNewCppScript(Wui::WuiContext& ctx){
		const bool isSystem = NewCppScriptIsSystem();
		// PECS-T11:「空」只写空白头文件 —— 不进类型账本、不登记 GameProject.cpp。
		const bool isEmpty = m_NewCppScriptKind == kCppScriptKindEmpty;
		const std::string name = CppScriptBaseName(m_NewCppScriptName);
		const std::string nameError = NewCppScriptNameError();
		if (!nameError.empty())
		{
			// 模态里已经画过行内错误;这条分支只是拒绝"绕过按钮的第二次调用"。
			m_NewCppScriptFailure = nameError;
			m_NewCppScriptFailureFor = NewCppScriptTargetPath().string();
			return false;
		}

		const std::filesystem::path target = NewCppScriptTargetPath();
		const std::filesystem::path parent = target.parent_path();
		std::error_code folderError;
		if (!std::filesystem::is_directory(parent, folderError))
			std::filesystem::create_directories(parent, folderError);

		std::string error;
		bool wrote = false;
		if (folderError)
		{
			error = Wui::Tr("modal.newscript.folder_failed", "Could not create the folder: ")
				+ parent.string();
		}
		else
		{
			// 临时文件 + 同目录改名(与脚本编辑器保存/新建着色器同一套写法);失败不留半成品。
			const std::filesystem::path temporary = parent / (target.filename().string() + ".tmp-write");
			std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
			if (!out.is_open())
			{
				error = Wui::Tr("modal.newscript.write_failed", "Could not write the file: ")
					+ temporary.string();
			}
			else
			{
				out << (isSystem ? Editor::GameProjectSystemHeaderSource(name)
					: (isEmpty ? NewCppEmptyTemplateSource(name) : NewCppScriptTemplateSource(name)));
				out.close();
				std::error_code renameError;
				std::filesystem::rename(temporary, target, renameError);
				if (renameError)
				{
					std::error_code cleanupError;
					std::filesystem::remove(temporary, cleanupError);
					error = Wui::Tr("modal.newscript.write_failed", "Could not write the file: ")
						+ target.string() + " (" + renameError.message() + ")";
				}
				else
				{
					wrote = true;
				}
			}
		}
		if (!wrote)
		{
			m_NewCppScriptFailure = error;
			m_NewCppScriptFailureFor = target.string();
			WLD_CORE_WARN("[new-cpp-script] write failed: {0}", error);
			return false;
		}

		// manifest 是**双向类型账本**(schema-compiler 拒绝"已声明但未登记"的新类型):
		// 创建入口把模板声明的三个类型(组件 / <Name>Data / <Name>Mode)全部登记,
		// 用户重建 Game 时生成器即可直接通过。手写新组件的作者仍需自己补这些行
		// (见 docs/user/scripting/README.md)。
		//
		// PROJ-8/T1:账本位置口径不变(组件源码的 `../Generated/Game.manifest`),组件落到项目层后
		// 它自动变成 `<项目根>/src/Generated/Game.manifest`(与 T2 的子项目构建故事一致)。
		// 新增一条**存在性守卫**:账本还不存在(项目从未构建过)时**不新建** —— 凭空写一份
		// 只有三条目的账本会把项目的构建输入改坏;由项目首次构建产出账本,之后再创建组件即可登记。
		const std::filesystem::path manifest =
			target.parent_path().parent_path() / "Generated" / "Game.manifest";
		std::error_code manifestFileError;
		// PECS-T8/T11:系统头文件与「空」头文件都不声明 schema 类型 ⇒ 不碰类型账本
		// (登记不存在的类型会让重建 Game 被拒)。
		if (isSystem)
		{
			WLD_CORE_INFO("[new-cpp-script] system '{0}' declares no schema types; type ledger untouched",
				name);
		}
		else if (isEmpty)
		{
			WLD_CORE_INFO("[new-cpp-script] empty header '{0}' declares no schema types; type ledger untouched",
				name);
		}
		else if (!std::filesystem::is_regular_file(manifest, manifestFileError))
		{
			WLD_CORE_WARN("[new-cpp-script] type ledger not found ({0}); "
				"skipping registration of {1}, {1}Data, {1}Mode", manifest.string(), name);
		}
		else
		{
			std::string manifestText;
			{
				std::ifstream in(manifest, std::ios::binary);
				std::string line;
				while (std::getline(in, line))
				{
					if (!line.empty() && line.back() == '\r')
						line.pop_back();
					manifestText += line;
					manifestText.push_back('\n');
				}
			}
			// 模板声明的**每个**类型都要登记(struct/enum 双向账本);漏一个重建 Game 就会被拒。
			const std::vector<std::string> entries = {
				"struct World::" + name,
				"struct World::" + name + "Data",
				"enum World::" + name + "Mode",
			};
			bool manifestChanged = false;
			for (const std::string& entry : entries)
			{
				if (manifestText.find(entry + "\n") != std::string::npos)
					continue;
				manifestText += entry;
				manifestText.push_back('\n');
				manifestChanged = true;
			}
			if (manifestChanged)
			{
				const std::filesystem::path temporary = manifest.string() + ".tmp-write";
				std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
				if (!out.is_open())
				{
					WLD_CORE_WARN("[new-cpp-script] could not update manifest '{0}'", manifest.string());
				}
				else
				{
					out << manifestText;
					out.close();
					std::error_code renameError;
					std::filesystem::rename(temporary, manifest, renameError);
					if (renameError)
					{
						std::error_code cleanupError;
						std::filesystem::remove(temporary, cleanupError);
						WLD_CORE_WARN("[new-cpp-script] could not update manifest '{0}': {1}",
							manifest.string(), renameError.message());
					}
				}
			}
		}

		// 落点/提示口径 = 相对**项目根**(项目源码视图的同一口径)。
		const std::string relative = CppScriptRelativePath(m_NewCppScriptKind, name);
		// 操作日志:与内容浏览器的新建资产(browser.new-shader)同一条口径。
		ctx.RecordOp("script", "new-cpp", name, relative);
		WLD_CORE_INFO("[new-cpp-script] created '{0}'", target.string());
		// CPPT-7/PROJ-8:C++ 源码一律外部 Visual Studio —— 走既有的"帧边界延迟打开"安全路径
		// (与内容浏览器双击脚本相同的安全点;EditorShell::OpenScriptEditorNow 按扩展名分流),
		// 内置脚本编辑器只服务 Lua/Luau。
		OpenScriptEditor(target.generic_string());
		if (isEmpty)
		{
			// PECS-T11:「空」不在 schema 发现目录、也没登记 —— 提示里必须说清楚"它现在不会生效",
			// 否则用户会以为建完就能进属性面板。
			PushNotice(Wui::TrFormat("notice.newscript.created.empty",
				"Created {path} — a blank header: it is not part of schema discovery and it is not "
				"registered in GameProject.cpp. Move it under src/Components/ (component) or "
				"src/Systems/ plus a GameProject.cpp registration (system) to make it live.",
				{ { "path", relative } }));
			return true;
		}
		if (!isSystem)
		{
			// 状态栏提示:编辑器不内置编译器 —— 显式告诉用户"用 VS 构建这个项目,再重载 C++ 模块"。
			PushNotice(Wui::TrFormat("notice.newscript.created",
				"Created {path} — build this project with Visual Studio (output under {project}/build), "
				"then use File ▶ Build & Reload C++ Module.",
				{ { "path", relative }, { "project", CurrentProjectRoot().generic_string() } }));
			return true;
		}

		// PECS-T8:系统类型额外把 include + 成对的两行登记进 <项目根>/src/GameProject.cpp。
		// 三种结果都要如实告诉用户 —— 尤其"头文件已落盘但没登记"绝不能静默(那正是本任务要消除的坑)。
		const Editor::GameProjectSystemRegistration registration =
			Editor::RegisterProjectSystem(CurrentProjectRoot(), name);
		if (!registration.Ok)
		{
			PushNotice(Wui::TrFormat("notice.newscript.register_failed",
				"Created {path}, but it could not be registered in GameProject.cpp: {error} — add one "
				"line inside AttachProjectSystems and one inside DetachProjectSystems manually.",
				{ { "path", relative }, { "error", registration.Error } }));
			WLD_CORE_WARN("[new-cpp-script] register failed: {0}", registration.Error);
			return true;
		}
		if (registration.AlreadyRegistered)
		{
			PushNotice(Wui::TrFormat("notice.newscript.register_already",
				"Created {path}; this system is already registered in GameProject.cpp.",
				{ { "path", relative } }));
			return true;
		}
		PushNotice(Wui::TrFormat("notice.newscript.registered",
			"Created {path} and registered it in GameProject.cpp (attached when the runtime starts, "
			"detached when it stops).",
			{ { "path", relative } }));
		return true;
	}


void EditorShell::DrawNewCppScriptModal(Wui::WuiContext& ctx){
		const Wui::WuiId modalId = Wui::HashId("modal.newscript");
		if (m_NewCppScriptOpen)
			ctx.SetModal(modalId);
		else if (ctx.Modal() == modalId)
			ctx.ClearModal();
		if (!m_NewCppScriptOpen)
			return;

		Wui::WuiRect frame;
		bool escapePressed = false;
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = Wui::Tr("modal.newscript.title", "New C++ …");
		frameDesc.Size = { 560.0f, 286.0f };
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, m_Theme))
		{
			// 模态被别的路径接管/收口:同步清掉宿主状态,避免状态与真实模态脱节。
			m_NewCppScriptOpen = false;
			return;
		}

		const float labelX = frame.X + 16.0f;
		const float fieldX = frame.X + 130.0f;
		const float suffixW = 30.0f;
		const float fieldW = frame.W - 146.0f - suffixW - 16.0f;
		const Wui::WuiId nameId = Wui::HashId("script.new.name");
		const Wui::WuiId okId = Wui::HashId("script.new.ok");
		const Wui::WuiId cancelId = Wui::HashId("script.new.cancel");
		const Wui::WuiId kindId = Wui::HashId("script.new.kind");
		const bool justOpened = ctx.Frame() == m_NewCppScriptOpenedFrame;

		// ---- 类型:组件(默认)/ 系统 / 空 —— 同一个向导三个落点(PECS-T8 + T11 任务 A)----
		// 下拉**显示**本地化名字(中文「组件 / 系统 / 空」);Combo 把当前选项串原样登记成 a11y value,
		// 所以画完再按同一 id 覆盖登记回英文术语(Component/System/Empty) —— 自动化口径不变。
		float cursorY = frame.Y + 46.0f;
		const std::string kindLabel = Wui::Tr("modal.newscript.kind", "Kind");
		Wui::Label(ctx, { labelX, cursorY + 5.0f }, kindLabel, m_Theme.TextMuted, 13.0f);
		int kindIndex = m_NewCppScriptKind;
		const std::vector<std::string> kindOptions {
			Wui::Tr(CppScriptKindLabelKey(kCppScriptKindComponent), kCppScriptKindComponentTerm),
			Wui::Tr(CppScriptKindLabelKey(kCppScriptKindSystem), kCppScriptKindSystemTerm),
			Wui::Tr(CppScriptKindLabelKey(kCppScriptKindEmpty), kCppScriptKindEmptyTerm),
		};
		const Wui::WuiRect kindRect { fieldX, cursorY, fieldW + suffixW, 24.0f };
		if (Wui::Combo(ctx, kindId, kindRect, kindLabel, kindOptions, kindIndex, m_Theme))
			m_NewCppScriptKind = kindIndex;
		// 无障碍 value 覆盖回英文术语(与下拉显示名解耦;同一 id 后写覆盖)。
		{
			Wui::WuiAccessNode node;
			node.Id = kindId;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = "shell";
			node.Kind = "combo";
			node.Label = kindLabel;
			node.Value = CppScriptKindTerm(m_NewCppScriptKind);
			node.Rect = kindRect;
			node.Enabled = true;
			node.Interactive = true;
			node.Focused = ctx.Focus() == kindId;
			Wui::WuiAccessibility::Get().Register(node);
		}
		cursorY += 34.0f;
		const bool isSystem = NewCppScriptIsSystem();
		const bool isEmpty = m_NewCppScriptKind == kCppScriptKindEmpty;

		// ---- 名称(标识符校验 + 重名拒绝都走同一条行内错误)----
		const std::string nameLabel = Wui::Tr("modal.newscript.name", "Name");
		Wui::Label(ctx, { labelX, cursorY + 5.0f }, nameLabel, m_Theme.TextMuted, 13.0f);
		const Wui::WuiRect nameRect { fieldX, cursorY, fieldW, 24.0f };
		// 写盘失败原因只对"同一个落点"有效:名字一改就作废(与新建着色器向导同口径)。
		if (!m_NewCppScriptFailure.empty() && m_NewCppScriptFailureFor != NewCppScriptTargetPath().string())
		{
			m_NewCppScriptFailure.clear();
			m_NewCppScriptFailureFor.clear();
		}
		const std::string nameError = NewCppScriptNameError();
		const std::string inlineError = nameError.empty() ? m_NewCppScriptFailure : nameError;
		Wui::TextFieldA11y nameA11y;
		nameA11y.Label = nameLabel;
		nameA11y.Placeholder = isSystem
			? Wui::Tr("modal.newscript.name.placeholder.system", "System name (valid C++ identifier)")
			: (isEmpty
				? Wui::Tr("modal.newscript.name.placeholder.empty", "File name (valid C++ identifier)")
				: Wui::Tr("modal.newscript.name.placeholder", "Component name (valid C++ identifier)"));
		// Enter 提交判定必须在**控件绘制前**取焦点:TextFieldCore 在回车那一帧会 `SetFocus(0)`
		// (提交即交出焦点),画完再读 ctx.Focus() 已经不是本字段(实测:回车点了不建文件)。
		const bool nameFocused = ctx.Focus() == nameId;
		// TextFieldEx:错误就地画在输入框下方(描边 Danger),同时把 error 追加进无障碍节点 value。
		const bool submitted = Wui::TextFieldEx(ctx, nameId, nameRect, m_NewCppScriptName, m_Theme,
			inlineError, &nameA11y);
		Wui::Label(ctx, { nameRect.X + nameRect.W + 8.0f, cursorY + 6.0f }, ".h", m_Theme.TextMuted, 13.0f);
		cursorY += 46.0f;

		// ---- 实时落点回显(相对当前项目根;绝对路径进节点 Tooltip)----
		const std::string relative =
			CppScriptRelativePath(m_NewCppScriptKind, CppScriptBaseName(m_NewCppScriptName));
		const std::string targetLabel = Wui::Tr("modal.newscript.target", "Will create");
		Wui::Label(ctx, { labelX, cursorY + 3.0f }, targetLabel, m_Theme.TextMuted, 12.0f);
		Wui::Label(ctx, { fieldX, cursorY + 1.0f }, relative, m_Theme.Text, 13.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("script.new.target");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = "shell";
			node.Kind = "text";
			node.Label = targetLabel;
			node.Value = relative;
			node.Tooltip = NewCppScriptTargetPath().generic_string();
			node.Rect = { fieldX, cursorY - 3.0f, fieldW + suffixW, 20.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		cursorY += 24.0f;

		// ---- 生效步骤提示(用 VS 构建项目 → 重载 C++ 模块;编辑器不内置编译器)----
		Wui::Label(ctx, { labelX, cursorY + 2.0f },
			isSystem
				? Wui::Tr("modal.newscript.hint.system",
					"The system is registered in <project>/src/GameProject.cpp automatically (attached when "
					"the runtime starts, detached when it stops). Build this project with Visual Studio "
					"(output under <project>/build), then File ▶ Build & Reload C++ Module.")
				: (isEmpty
					? Wui::Tr("modal.newscript.hint.empty",
						"A blank header under <project>/src/ — it is not part of schema discovery and is "
						"not registered, so building alone will not put it in the property panel.")
					: Wui::Tr("modal.newscript.hint",
						"Build this project with Visual Studio (output under <project>/build), "
						"then File ▶ Build & Reload C++ Module.")),
			m_Theme.TextMuted, 12.0f);

		// ---- 底部按钮(名称不合法/重名时创建按钮禁用并带原因)----
		const bool canCreate = nameError.empty();
		const Wui::ModalResult footerResult = Wui::ModalFooter(ctx, frame,
			Wui::Tr("modal.newscript.ok", "Create"),
			Wui::Tr("modal.newscript.cancel", "Cancel"),
			okId, cancelId, canCreate, m_Theme);

		bool closeRequested = false;
		bool created = false;
		if ((footerResult == Wui::ModalResult::Confirm || (submitted && nameFocused && !justOpened))
			&& canCreate)
		{
			created = CreateNewCppScript(ctx);
			closeRequested = created;
		}
		else if (footerResult == Wui::ModalResult::Cancel || escapePressed)
		{
			ctx.RecordOp("script", "new-cpp-cancel", CppScriptBaseName(m_NewCppScriptName), relative);
			closeRequested = true;
		}

		if (closeRequested)
		{
			m_NewCppScriptOpen = false;
			m_NewCppScriptFailure.clear();
			m_NewCppScriptFailureFor.clear();
			ctx.ClearModal();
		}
		Wui::EndModalFrame(ctx);
		// 创建成功:操作日志/提示/打开编辑器都在 CreateNewCppScript 里完成(单一出口)。
		(void)created;
	}


	// ---- PECS-T9/T11:File ▸ New Lua …(一个 Lua 向导 + 类型下拉;与 C++ 向导同构)----
	// 三类落点(都相对**内容根**):
	//   * 系统   → `scripts/systems/<Name>.luau` —— 唯一会被 Play 自动装载的目录;
	//     模板优先磁盘上的 `scripts/templates/WorldScript.lua`(与 Scripts 面板/内容浏览器
	//     同一份),缺失时用内置骨架(规范签名 `ecs:AddSystem(名字, 函数, 阶段)`);
	//   * 脚本库 → `scripts/lib/<Name>.luau` —— 不自动加载,内置骨架 `local M = {} … return M`;
	//   * 空     → `scripts/<Name>.luau`   —— 不自动加载,内置骨架只有注释。
std::filesystem::path EditorShell::NewLuaSystemTargetPath() const{
		// PECS-T9/T11:内容根取运行期 World::Paths::AssetRoot(),与进程 CWD 无关。
		// 只有 scripts/systems/ 下的脚本会被 Play 自动装载 —— "脚本库 / 空"两类因此**不生效**
		// 到自动加载(这正是向导要在提示里说清楚的事)。
		return World::Paths::AssetRoot()
			/ std::filesystem::path(LuaScriptRelativePath(m_NewLuaSystemKind,
				LuaSystemBaseName(m_NewLuaSystemName)));
	}


std::string EditorShell::NewLuaSystemNameError() const{
		// 没有当前项目 / 没有内容根时先给"先打开或新建项目"这条可读原因,不再去拼一个相对路径
		// (那会按 CWD 误判"已存在")。
		if (CurrentProjectRoot().empty())
			return Wui::Tr("modal.newlua.no_project",
				"No project is open — open or create a project in the launcher first.");
		if (World::Paths::AssetRoot().empty())
			return Wui::Tr("modal.newlua.no_content_root",
				"The project has no content root — expected <project>/assets.");
		const std::string name = LuaSystemBaseName(m_NewLuaSystemName);
		if (name.empty())
			return Wui::Tr("modal.newlua.name.empty", "Name cannot be empty");
		if (!IsValidLuaScriptFileName(name))
			return Wui::Tr("modal.newlua.name.invalid",
				"Name cannot contain \\ / : * ? \" < > | and cannot end with a dot or a space");
		std::error_code existsError;
		if (std::filesystem::exists(NewLuaSystemTargetPath(), existsError))
		{
			const std::string relative = LuaScriptRelativePath(m_NewLuaSystemKind, name);
			return Wui::TrFormat("modal.newlua.name.exists",
				"A file with this name already exists: {path}", { { "path", relative } });
		}
		return {};
	}


void EditorShell::OpenNewLuaSystemModal(Wui::WuiContext& ctx){
		// 没有当前项目时不打开一个写不了盘的模态 —— 给一条可读提示,并把这次意图记进操作日志
		// (与 T8 的「新建 C++ …」向导同口径,方便自动化断言"点过但被拦下")。
		if (CurrentProjectRoot().empty() || World::Paths::AssetRoot().empty())
		{
			PushNotice(Wui::Tr("notice.newlua.no_project",
				"No project is open — open or create a project in the launcher first, "
				"then create Lua systems."));
			ctx.RecordOp("script", "new-lua-no-project", m_NewLuaSystemName, "");
			WLD_CORE_WARN("[new-lua-system] rejected: no current project");
			return;
		}
		m_NewLuaSystemOpen = true;
		m_NewLuaSystemOpenedFrame = static_cast<uint32_t>(ctx.Frame());
		// PECS-T11:每次打开都回到默认类型 = 系统(T9 的行为不变)。
		m_NewLuaSystemKind = kLuaScriptKindSystem;
		m_NewLuaSystemName = "MySystem";
		m_NewLuaSystemFailure.clear();
		m_NewLuaSystemFailureFor.clear();
		ctx.SetModal(Wui::HashId("modal.newlua"));
		ctx.SetFocus(Wui::HashId("lua.new.name"));
		ctx.RecordOp("script", "new-lua-ask", m_NewLuaSystemName, LuaScriptRelativeDir(m_NewLuaSystemKind));
	}


bool EditorShell::CreateNewLuaSystem(Wui::WuiContext& ctx){
		const int kind = m_NewLuaSystemKind;
		const std::string name = LuaSystemBaseName(m_NewLuaSystemName);
		const std::string nameError = NewLuaSystemNameError();
		if (!nameError.empty())
		{
			// 模态里已经画过行内错误;这条分支只是拒绝"绕过按钮的第二次调用"。
			m_NewLuaSystemFailure = nameError;
			m_NewLuaSystemFailureFor = NewLuaSystemTargetPath().string();
			return false;
		}

		const std::filesystem::path target = NewLuaSystemTargetPath();
		const std::filesystem::path parent = target.parent_path();
		std::error_code folderError;
		if (!std::filesystem::is_directory(parent, folderError))
			std::filesystem::create_directories(parent, folderError);

		std::string error;
		bool wrote = false;
		if (folderError)
		{
			error = Wui::Tr("modal.newlua.folder_failed", "Could not create the folder: ")
				+ parent.string();
		}
		else
		{
			// 模板优先:只有**系统**类用磁盘模板 `<内容根>/scripts/templates/WorldScript.lua`
			// 逐字节复制(与 Scripts 面板同一份);库 / 空用各自的内置骨架。失败不留半成品。
			const std::filesystem::path templatePath =
				World::Paths::AssetRoot() / "scripts" / "templates" / "WorldScript.lua";
			std::error_code templateError;
			if (kind == kLuaScriptKindSystem
				&& std::filesystem::is_regular_file(templatePath, templateError))
			{
				std::error_code copyError;
				std::filesystem::copy_file(templatePath, target,
					std::filesystem::copy_options::none, copyError);
				if (copyError)
				{
					error = Wui::Tr("modal.newlua.write_failed", "Could not write the file: ")
						+ target.string() + " (" + copyError.message() + ")";
				}
				else
				{
					wrote = true;
				}
			}
			else
			{
				const std::filesystem::path temporary = parent / (target.filename().string() + ".tmp-write");
				std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
				if (!out.is_open())
				{
					error = Wui::Tr("modal.newlua.write_failed", "Could not write the file: ")
						+ temporary.string();
				}
				else
				{
					out << (kind == kLuaScriptKindLibrary ? NewLuaLibraryTemplateSource()
						: (kind == kLuaScriptKindEmpty ? NewLuaEmptyTemplateSource()
							: NewLuaSystemTemplateSource()));
					out.close();
					std::error_code renameError;
					std::filesystem::rename(temporary, target, renameError);
					if (renameError)
					{
						std::error_code cleanupError;
						std::filesystem::remove(temporary, cleanupError);
						error = Wui::Tr("modal.newlua.write_failed", "Could not write the file: ")
							+ target.string() + " (" + renameError.message() + ")";
					}
					else
					{
						wrote = true;
					}
				}
			}
		}
		if (!wrote)
		{
			m_NewLuaSystemFailure = error;
			m_NewLuaSystemFailureFor = target.string();
			WLD_CORE_WARN("[new-lua-system] write failed: {0}", error);
			return false;
		}

		// 落点/提示口径 = 相对**内容根**(脚本编辑器的逻辑路径同一口径)。
		const std::string relative = LuaScriptRelativePath(kind, name);
		ctx.RecordOp("script", "new-lua", name, relative);
		WLD_CORE_INFO("[new-lua-system] created '{0}'", target.string());
		// 如果面板此前已在注册表中(例如启动时从布局恢复、但当时磁盘文件尚不存在),刷新从磁盘载入
		const std::string panelId = std::string(kScriptPanelPrefix) + relative;
		const auto foundPanel = m_PanelRegistry.find(panelId);
		if (foundPanel != m_PanelRegistry.end() && foundPanel->second)
		{
			if (auto* scriptPanel = dynamic_cast<ScriptEditorPanel*>(foundPanel->second.get()))
			{
				scriptPanel->LoadFromDisk();
			}
		}
		// 脚本编辑器接受**相对内容根的逻辑路径**(内置编辑器服务 Lua/Luau;与内容浏览器双击同一路径)。
		OpenScriptEditor(relative);
		if (kind == kLuaScriptKindLibrary)
		{
			PushNotice(Wui::TrFormat("notice.newlua.created.library",
				"Created {path} — a script library under scripts/lib/; it is NOT auto-loaded on Play.",
				{ { "path", relative } }));
		}
		else if (kind == kLuaScriptKindEmpty)
		{
			PushNotice(Wui::TrFormat("notice.newlua.created.empty",
				"Created {path} — it is NOT auto-loaded: only scripts/systems/ is loaded on Play.",
				{ { "path", relative } }));
		}
		else
		{
			PushNotice(Wui::TrFormat("notice.newlua.created",
				"Created {path} — it loads automatically when you press Play (scripts/systems/).",
				{ { "path", relative } }));
		}
		return true;
	}


void EditorShell::DrawNewLuaSystemModal(Wui::WuiContext& ctx){
		const Wui::WuiId modalId = Wui::HashId("modal.newlua");
		if (m_NewLuaSystemOpen)
			ctx.SetModal(modalId);
		else if (ctx.Modal() == modalId)
			ctx.ClearModal();
		if (!m_NewLuaSystemOpen)
			return;

		Wui::WuiRect frame;
		bool escapePressed = false;
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = Wui::Tr("modal.newlua.title", "New Lua …");
		frameDesc.Size = { 560.0f, 272.0f };
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, m_Theme))
		{
			// 模态被别的路径接管/收口:同步清掉宿主状态,避免状态与真实模态脱节。
			m_NewLuaSystemOpen = false;
			return;
		}

		const float labelX = frame.X + 16.0f;
		const float fieldX = frame.X + 130.0f;
		const float suffixW = 30.0f;
		const float fieldW = frame.W - 146.0f - suffixW - 16.0f;
		const Wui::WuiId nameId = Wui::HashId("lua.new.name");
		const Wui::WuiId okId = Wui::HashId("lua.new.ok");
		const Wui::WuiId cancelId = Wui::HashId("lua.new.cancel");
		const Wui::WuiId kindId = Wui::HashId("lua.new.kind");
		const bool justOpened = ctx.Frame() == m_NewLuaSystemOpenedFrame;

		// ---- 类型:系统(默认)/ 脚本库 / 空 —— 与 C++ 向导同构(显示本地化,a11y value 英文术语)----
		float cursorY = frame.Y + 46.0f;
		const std::string kindLabel = Wui::Tr("modal.newlua.kind", "Kind");
		Wui::Label(ctx, { labelX, cursorY + 5.0f }, kindLabel, m_Theme.TextMuted, 13.0f);
		int kindIndex = m_NewLuaSystemKind;
		const std::vector<std::string> kindOptions {
			Wui::Tr(LuaScriptKindLabelKey(kLuaScriptKindSystem), kLuaScriptKindSystemTerm),
			Wui::Tr(LuaScriptKindLabelKey(kLuaScriptKindLibrary), kLuaScriptKindLibraryTerm),
			Wui::Tr(LuaScriptKindLabelKey(kLuaScriptKindEmpty), kLuaScriptKindEmptyTerm),
		};
		const Wui::WuiRect kindRect { fieldX, cursorY, fieldW + suffixW, 24.0f };
		if (Wui::Combo(ctx, kindId, kindRect, kindLabel, kindOptions, kindIndex, m_Theme))
			m_NewLuaSystemKind = kindIndex;
		{
			Wui::WuiAccessNode node;
			node.Id = kindId;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = "shell";
			node.Kind = "combo";
			node.Label = kindLabel;
			node.Value = LuaScriptKindTerm(m_NewLuaSystemKind);
			node.Rect = kindRect;
			node.Enabled = true;
			node.Interactive = true;
			node.Focused = ctx.Focus() == kindId;
			Wui::WuiAccessibility::Get().Register(node);
		}
		cursorY += 34.0f;
		const int kind = m_NewLuaSystemKind;

		// ---- 名称(空/非法文件名/重名都走同一条行内错误)----
		const std::string nameLabel = Wui::Tr("modal.newlua.name", "Name");
		Wui::Label(ctx, { labelX, cursorY + 5.0f }, nameLabel, m_Theme.TextMuted, 13.0f);
		const Wui::WuiRect nameRect { fieldX, cursorY, fieldW, 24.0f };
		// 写盘失败原因只对"同一个落点"有效:名字一改就作废(与 T8 向导同口径)。
		if (!m_NewLuaSystemFailure.empty() && m_NewLuaSystemFailureFor != NewLuaSystemTargetPath().string())
		{
			m_NewLuaSystemFailure.clear();
			m_NewLuaSystemFailureFor.clear();
		}
		const std::string nameError = NewLuaSystemNameError();
		const std::string inlineError = nameError.empty() ? m_NewLuaSystemFailure : nameError;
		Wui::TextFieldA11y nameA11y;
		nameA11y.Label = nameLabel;
		nameA11y.Placeholder = kind == kLuaScriptKindLibrary
			? Wui::Tr("modal.newlua.name.placeholder.library", "Library name (used as the file name)")
			: (kind == kLuaScriptKindEmpty
				? Wui::Tr("modal.newlua.name.placeholder.empty", "File name (used as the file name)")
				: Wui::Tr("modal.newlua.name.placeholder", "System name (used as the file name)"));
		// Enter 提交判定必须在**控件绘制前**取焦点(与 T8 向导同一条实测纪律)。
		const bool nameFocused = ctx.Focus() == nameId;
		const bool submitted = Wui::TextFieldEx(ctx, nameId, nameRect, m_NewLuaSystemName, m_Theme,
			inlineError, &nameA11y);
		Wui::Label(ctx, { nameRect.X + nameRect.W + 8.0f, cursorY + 6.0f }, ".luau", m_Theme.TextMuted, 13.0f);
		cursorY += 46.0f;

		// ---- 实时落点回显(相对内容根;绝对路径进节点 Tooltip)----
		const std::string relative =
			LuaScriptRelativePath(kind, LuaSystemBaseName(m_NewLuaSystemName));
		const std::string targetLabel = Wui::Tr("modal.newlua.target", "Will create");
		Wui::Label(ctx, { labelX, cursorY + 3.0f }, targetLabel, m_Theme.TextMuted, 12.0f);
		Wui::Label(ctx, { fieldX, cursorY + 1.0f }, relative, m_Theme.Text, 13.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("lua.new.target");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = "shell";
			node.Kind = "text";
			node.Label = targetLabel;
			node.Value = relative;
			node.Tooltip = NewLuaSystemTargetPath().generic_string();
			node.Rect = { fieldX, cursorY - 3.0f, fieldW + suffixW, 20.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		cursorY += 24.0f;

		// ---- 生效步骤提示(脚本系统不走编译;Play 时自动加载)----
		Wui::Label(ctx, { labelX, cursorY + 2.0f },
			kind == kLuaScriptKindLibrary
				? Wui::Tr("modal.newlua.hint.library",
					"Saved under scripts/lib/ — it is NOT auto-loaded on Play; keep reusable code here.")
				: (kind == kLuaScriptKindEmpty
					? Wui::Tr("modal.newlua.hint.empty",
						"Saved under scripts/ — it is NOT auto-loaded: only scripts/systems/ is loaded "
						"when you press Play.")
					: Wui::Tr("modal.newlua.hint",
						"Saved under scripts/systems/ — it is loaded automatically when you press Play, "
						"no build step needed.")),
			m_Theme.TextMuted, 12.0f);

		// ---- 底部按钮(名称非法/重名时创建按钮禁用并带原因)----
		const bool canCreate = nameError.empty();
		const Wui::ModalResult footerResult = Wui::ModalFooter(ctx, frame,
			Wui::Tr("modal.newlua.ok", "Create"),
			Wui::Tr("modal.newlua.cancel", "Cancel"),
			okId, cancelId, canCreate, m_Theme);

		bool closeRequested = false;
		bool created = false;
		if ((footerResult == Wui::ModalResult::Confirm || (submitted && nameFocused && !justOpened))
			&& canCreate)
		{
			created = CreateNewLuaSystem(ctx);
			closeRequested = created;
		}
		else if (footerResult == Wui::ModalResult::Cancel || escapePressed)
		{
			ctx.RecordOp("script", "new-lua-cancel", LuaSystemBaseName(m_NewLuaSystemName), relative);
			closeRequested = true;
		}

		if (closeRequested)
		{
			m_NewLuaSystemOpen = false;
			m_NewLuaSystemFailure.clear();
			m_NewLuaSystemFailureFor.clear();
			ctx.ClearModal();
		}
		Wui::EndModalFrame(ctx);
		// 创建成功:操作日志/提示/打开编辑器都在 CreateNewLuaSystem 里完成(单一出口)。
		(void)created;
	}

}
