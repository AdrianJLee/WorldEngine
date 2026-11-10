#include "wldpch.h"
#include "World/Math/Curve/CurveLibrary.h"

namespace World
{
	CurveLibrary& CurveLibrary::Get()
	{
		static CurveLibrary s_Instance;
		return s_Instance;
	}

	std::shared_ptr<CubicBezierCurve> CurveLibrary::Load(const std::string& logicalPath)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		auto it = m_Curves.find(logicalPath);
		if (it != m_Curves.end() && it->second)
			return it->second;

		auto curve = std::make_shared<CubicBezierCurve>();
		if (!curve->LoadFromFile(logicalPath))
			return nullptr;

		m_Curves[logicalPath] = curve;
		return curve;
	}

	std::shared_ptr<CubicBezierCurve> CurveLibrary::Get(const std::string& logicalPath)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		auto it = m_Curves.find(logicalPath);
		if (it != m_Curves.end() && it->second)
			return it->second;

		auto curve = std::make_shared<CubicBezierCurve>();
		if (curve->LoadFromFile(logicalPath))
		{
			m_Curves[logicalPath] = curve;
			return curve;
		}
		return nullptr;
	}

	void CurveLibrary::Reload(const std::string& logicalPath)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		auto it = m_Curves.find(logicalPath);
		if (it != m_Curves.end() && it->second)
		{
			it->second->LoadFromFile(logicalPath);
		}
		else
		{
			auto curve = std::make_shared<CubicBezierCurve>();
			if (curve->LoadFromFile(logicalPath))
			{
				m_Curves[logicalPath] = curve;
			}
		}
	}

	void CurveLibrary::Clear()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Curves.clear();
	}
}
