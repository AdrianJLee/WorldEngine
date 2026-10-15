#pragma once

#include <string>
#include <vector>

namespace World::Ai
{
	class CommandRegistry;
}

namespace World::Assistant
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

		// 注册命令到 CommandRegistry
		static void RegisterCommands(World::Ai::CommandRegistry& reg);
	};
}
