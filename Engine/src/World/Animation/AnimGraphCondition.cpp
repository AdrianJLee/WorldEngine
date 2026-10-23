#include "wldpch.h"

#include "World/Animation/AnimGraphCondition.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace World
{
	namespace AnimGraphCondition
	{
		namespace
		{
			enum class CompareOp : uint8_t
			{
				Greater,
				GreaterEqual,
				Less,
				LessEqual,
				Equal,
				NotEqual
			};

			struct Term
			{
				std::string Name;
				CompareOp Op = CompareOp::Greater;
				float Value = 0.0f;
			};

			struct Parser
			{
				std::string_view Text;
				size_t Cursor = 0;
				std::string Error;

				void SkipSpaces()
				{
					while (Cursor < Text.size() && std::isspace(static_cast<unsigned char>(Text[Cursor])))
						++Cursor;
				}

				bool Consume(std::string_view token)
				{
					SkipSpaces();
					if (Text.substr(Cursor, token.size()) != token)
						return false;
					Cursor += token.size();
					return true;
				}

				bool Peek(std::string_view token)
				{
					SkipSpaces();
					return Text.substr(Cursor, token.size()) == token;
				}

				bool ParseIdentifier(std::string& out)
				{
					SkipSpaces();
					const size_t start = Cursor;
					if (Cursor >= Text.size())
						return false;
					const unsigned char first = static_cast<unsigned char>(Text[Cursor]);
					if (!std::isalpha(first) && first != '_')
						return false;
					while (Cursor < Text.size())
					{
						const unsigned char c = static_cast<unsigned char>(Text[Cursor]);
						if (!std::isalnum(c) && c != '_' && c != '.')
							break;
						++Cursor;
					}
					out.assign(Text.substr(start, Cursor - start));
					return !out.empty();
				}

				bool ParseNumber(float& out)
				{
					SkipSpaces();
					if (Cursor >= Text.size())
						return false;
					// string_view 不保证 0 结尾：复制剩余片段再交给 strtof。
					const std::string buffer(Text.substr(Cursor));
					const char* begin = buffer.c_str();
					char* end = nullptr;
					const float parsed = std::strtof(begin, &end);
					if (end == begin)
						return false;
					Cursor += static_cast<size_t>(end - begin);
					out = parsed;
					return std::isfinite(out);
				}

				bool ParseCompareOp(CompareOp& out)
				{
					SkipSpaces();
					if (Consume(">=")) { out = CompareOp::GreaterEqual; return true; }
					if (Consume("<=")) { out = CompareOp::LessEqual; return true; }
					if (Consume("==")) { out = CompareOp::Equal; return true; }
					if (Consume("!=")) { out = CompareOp::NotEqual; return true; }
					if (Consume(">")) { out = CompareOp::Greater; return true; }
					if (Consume("<")) { out = CompareOp::Less; return true; }
					return false;
				}

				bool ParseTerm(Term& out)
				{
					if (!ParseIdentifier(out.Name))
					{
						Error = "expected parameter name";
						return false;
					}
					if (!ParseCompareOp(out.Op))
					{
						Error = "expected comparison operator after '" + out.Name + "'";
						return false;
					}
					if (!ParseNumber(out.Value))
					{
						Error = "expected numeric literal after '" + out.Name + "'";
						return false;
					}
					return true;
				}

				// expression := andGroup ('||' andGroup)*
				// andGroup   := term ('&&' term)*
				bool ParseAll(std::vector<std::vector<Term>>& outGroups)
				{
					while (true)
					{
						std::vector<Term> group;
						Term term;
						if (!ParseTerm(term))
							return false;
						group.push_back(term);
						while (Peek("&&"))
						{
							Consume("&&");
							if (!ParseTerm(term))
								return false;
							group.push_back(term);
						}
						outGroups.push_back(std::move(group));
						SkipSpaces();
						if (Cursor >= Text.size())
							break;
						if (!Consume("||"))
						{
							Error = "unexpected token after expression";
							return false;
						}
					}
					return true;
				}
			};

			bool EvaluateTerm(const Term& term, const ParameterMap& params)
			{
				float left = 0.0f;
				const auto it = params.find(term.Name);
				if (it != params.end())
					left = it->second;
				if (!std::isfinite(left) || !std::isfinite(term.Value))
					return false;

				switch (term.Op)
				{
				case CompareOp::Greater: return left > term.Value;
				case CompareOp::GreaterEqual: return left >= term.Value;
				case CompareOp::Less: return left < term.Value;
				case CompareOp::LessEqual: return left <= term.Value;
				case CompareOp::Equal: return std::fabs(left - term.Value) <= 1e-6f;
				case CompareOp::NotEqual: return std::fabs(left - term.Value) > 1e-6f;
				}
				return false;
			}
		}

		Predicate Parse(std::string_view expression, std::string* error)
		{
			Parser parser;
			parser.Text = expression;
			std::vector<std::vector<Term>> groups;
			if (!parser.ParseAll(groups) || groups.empty())
			{
				if (error)
					*error = parser.Error.empty() ? std::string("empty condition expression") : parser.Error;
				return {};
			}

			return [groups = std::move(groups)](const ParameterMap& params)
			{
				// && 组内全真；任一组为真 ⇒ 整体为真（&& 优先级高于 ||）。
				for (const auto& group : groups)
				{
					bool all = true;
					for (const Term& term : group)
					{
						if (!EvaluateTerm(term, params))
						{
							all = false;
							break;
						}
					}
					if (all)
						return true;
				}
				return false;
			};
		}
	}
}
