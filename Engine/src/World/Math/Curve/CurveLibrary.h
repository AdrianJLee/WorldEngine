#pragma once

#include "World/Core/Export.h"
#include "World/Math/Curve/CubicBezierCurve.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace World
{
	class WLD_API CurveLibrary
	{
	public:
		static CurveLibrary& Get();

		std::shared_ptr<CubicBezierCurve> Load(const std::string& logicalPath);
		std::shared_ptr<CubicBezierCurve> Get(const std::string& logicalPath);
		void Reload(const std::string& logicalPath);
		void Clear();

	private:
		CurveLibrary() = default;
		~CurveLibrary() = default;
		CurveLibrary(const CurveLibrary&) = delete;
		CurveLibrary& operator=(const CurveLibrary&) = delete;

		mutable std::mutex m_Mutex;
		std::unordered_map<std::string, std::shared_ptr<CubicBezierCurve>> m_Curves;
	};
}
