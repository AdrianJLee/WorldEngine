#pragma once

#include <entt.hpp>
#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace World
{
	namespace Detail
	{
		template<typename... Ts>
		struct TypeList {};

		template<typename T, typename List>
		struct TypeListContains;

		template<typename T, typename... Ts>
		struct TypeListContains<T, TypeList<Ts...>> : std::disjunction<std::is_same<T, Ts>...> {};

		template<typename T, typename List>
		inline constexpr bool TypeListContains_v = TypeListContains<T, List>::value;

		template<typename List, typename T>
		struct TypeListAppendUnique;

		template<typename... Ts, typename T>
		struct TypeListAppendUnique<TypeList<Ts...>, T>
		{
			using type = std::conditional_t<
				TypeListContains_v<T, TypeList<Ts...>>,
				TypeList<Ts...>,
				TypeList<Ts..., T>
			>;
		};

		template<typename List1, typename List2>
		struct TypeListConcatUnique;

		template<typename List1>
		struct TypeListConcatUnique<List1, TypeList<>>
		{
			using type = List1;
		};

		template<typename List1, typename Head, typename... Tail>
		struct TypeListConcatUnique<List1, TypeList<Head, Tail...>>
		{
			using Appended = typename TypeListAppendUnique<List1, Head>::type;
			using type = typename TypeListConcatUnique<Appended, TypeList<Tail...>>::type;
		};

		template<typename List1, typename List2>
		using TypeListConcatUnique_t = typename TypeListConcatUnique<List1, List2>::type;
	}

	template<typename ComponentList, typename ExtraList = Detail::TypeList<>, typename ExcludeList = Detail::TypeList<>>
	class BasicQuery;

	template<typename... Components, typename... ExtraComponents, typename... ExcludeComponents>
	class BasicQuery<Detail::TypeList<Components...>, Detail::TypeList<ExtraComponents...>, Detail::TypeList<ExcludeComponents...>>
	{
	public:
		explicit BasicQuery(entt::registry& registry)
			: m_Registry(&registry)
		{
		}

		explicit BasicQuery(const entt::registry& registry)
			: m_Registry(const_cast<entt::registry*>(&registry))
		{
		}

		template<typename... Extra>
		auto With() const
		{
			using NewExtraList = Detail::TypeListConcatUnique_t<
				Detail::TypeList<ExtraComponents...>,
				Detail::TypeList<Extra...>
			>;
			return BasicQuery<
				Detail::TypeList<Components...>,
				NewExtraList,
				Detail::TypeList<ExcludeComponents...>
			>(*m_Registry);
		}

		template<typename... Exclude>
		auto Without() const
		{
			using NewExcludeList = Detail::TypeListConcatUnique_t<
				Detail::TypeList<ExcludeComponents...>,
				Detail::TypeList<std::remove_const_t<Exclude>...>
			>;
			return BasicQuery<
				Detail::TypeList<Components...>,
				Detail::TypeList<ExtraComponents...>,
				NewExcludeList
			>(*m_Registry);
		}

		template<typename Func>
		void Each(Func&& func) const
		{
			auto view = GetView();

			if constexpr (std::is_invocable_v<Func, Components&...>)
			{
				for (const auto entity : view)
				{
					func(view.template get<Components>(entity)...);
				}
			}
			else if constexpr (std::is_invocable_v<Func, entt::entity, Components&...>)
			{
				for (const auto entity : view)
				{
					func(entity, view.template get<Components>(entity)...);
				}
			}
			else
			{
				static_assert(
					std::is_invocable_v<Func, Components&...> ||
					std::is_invocable_v<Func, entt::entity, Components&...>,
					"Callable provided to Query::Each must accept (Components&...) or (entt::entity, Components&...)");
			}
		}

		[[nodiscard]] std::size_t Count() const
		{
			auto view = GetView();
			std::size_t count = 0;
			for (const auto entity : view)
			{
				(void)entity;
				++count;
			}
			return count;
		}

		[[nodiscard]] bool Empty() const
		{
			auto view = GetView();
			return view.begin() == view.end();
		}

		auto begin() const { return GetView().begin(); }
		auto end() const { return GetView().end(); }

	private:
		auto GetView() const
		{
			return GetViewImpl(Detail::TypeList<ExtraComponents...>{}, Detail::TypeList<ExcludeComponents...>{});
		}

		template<typename... Extra, typename... Exclude>
		auto GetViewImpl(Detail::TypeList<Extra...>, Detail::TypeList<Exclude...>) const
		{
			if constexpr (sizeof...(Exclude) == 0)
			{
				return m_Registry->template view<Components..., Extra...>();
			}
			else
			{
				return m_Registry->template view<Components..., Extra...>(entt::exclude<Exclude...>);
			}
		}

	protected:
		entt::registry* m_Registry = nullptr;
	};

	template<typename... Components>
	class Query : public BasicQuery<Detail::TypeList<Components...>, Detail::TypeList<>, Detail::TypeList<>>
	{
		static_assert(sizeof...(Components) > 0, "World::Query requires at least one component type.");

	public:
		using Base = BasicQuery<Detail::TypeList<Components...>, Detail::TypeList<>, Detail::TypeList<>>;
		using Base::Base;
	};
}
