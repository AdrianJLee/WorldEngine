#pragma once

#include <string>
#include <vector>

namespace World::Ai
{
	class CommandRegistry;
}

namespace World::Studio
{
	struct QuestionOption
	{
		std::string id;
		std::string label;
		std::string description;
		bool isRecommended = false;
	};

	struct InterviewQuestion
	{
		std::string id;
		std::string header;
		std::string prompt;
		std::vector<QuestionOption> options;
		std::string selectedOptionId;

		std::string ToJson() const;
	};

	struct InterviewRound
	{
		int round = 1;
		std::vector<InterviewQuestion> questions; // 最多 3 个问题

		std::string ToJson() const;
	};

	class InterviewSystem
	{
	public:
		static InterviewSystem& Get();

		// 获取当前阶段的制作人访谈卡片 (严格保证每轮 <= 3 题，每题带选项与推荐)
		InterviewRound GetCurrentRound(const std::string& projectRoot, int stage = 0);

		// 回答访谈问题，若采纳推荐则沉淀进入 assumptions.md，偏好沉淀至 taste.yaml
		bool AnswerQuestion(const std::string& projectRoot, const std::string& questionId, const std::string& optionId);

		// 获取项目品味档案
		std::string GetTasteProfile(const std::string& projectRoot) const;

		// 注册命令到 CommandRegistry (包含 studio.* 与 assistant.* 兼容别名)
		static void RegisterCommands(World::Ai::CommandRegistry& reg);
	};
}

namespace World::Assistant
{
	using World::Studio::QuestionOption;
	using World::Studio::InterviewQuestion;
	using World::Studio::InterviewRound;
	using World::Studio::InterviewSystem;
}
