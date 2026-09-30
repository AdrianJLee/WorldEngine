#include "wldpch.h"

#include "VisualStudioAutomation.h"

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <iterator>
#include <memory>
#include <string.h>
#include <thread>

namespace World::Editor
{
	namespace
	{
		// ------------------------------------------------------------------
		// 限时执行:把可能阻塞的 COM 工作放到 worker 线程。
		//
		// 背景:VS 弹着模态对话框时 DTE 调用会挂住调用线程 —— 编辑器 UI 线程绝不能被它卡住。
		// 语义:**超时 = 无结果**(调用方走回落),因此超时后不读仍可能被写入的结果,
		// worker 由 shared_ptr 持有全部状态并自行收尾(detach 安全)。
		// ------------------------------------------------------------------
		template <typename T>
		bool RunComWorkWithTimeout(const std::function<void(T&)>& work, T& out, unsigned timeoutMs)
		{
			struct Shared
			{
				std::atomic<bool> Done { false };
				T Value {};
			};
			auto shared = std::make_shared<Shared>();
			std::thread([shared, work]()
			{
				const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
				const bool uninit = SUCCEEDED(init);
				work(shared->Value);
				if (uninit)
					::CoUninitialize();
				shared->Done.store(true, std::memory_order_release);
			}).detach();

			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
			while (!shared->Done.load(std::memory_order_acquire))
			{
				if (std::chrono::steady_clock::now() >= deadline)
					return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			out = shared->Value;
			return true;
		}

		// ---- IDispatch 晚期绑定(不引入 VS SDK / typelib,版本无关)----
		bool GetPropertyVariant(IDispatch* object, const wchar_t* name, VARIANT* out)
		{
			if (!object || !name || !out)
				return false;
			DISPID id = 0;
			LPOLESTR names[] = { const_cast<LPOLESTR>(name) };
			if (FAILED(object->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id)))
				return false;
			DISPPARAMS parameters { nullptr, nullptr, 0, 0 };
			return SUCCEEDED(object->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
				DISPATCH_PROPERTYGET, &parameters, out, nullptr, nullptr));
		}

		IDispatch* GetDispatchProperty(IDispatch* object, const wchar_t* name)
		{
			VARIANT value;
			VariantInit(&value);
			IDispatch* result = nullptr;
			if (GetPropertyVariant(object, name, &value))
			{
				if (value.vt == VT_DISPATCH && value.pdispVal)
					result = value.pdispVal;   // 转移所有权(不 Release)
				else
					VariantClear(&value);
			}
			return result;
		}

		std::wstring GetStringProperty(IDispatch* object, const wchar_t* name)
		{
			VARIANT value;
			VariantInit(&value);
			std::wstring text;
			if (GetPropertyVariant(object, name, &value))
			{
				if (value.vt == VT_BSTR && value.bstrVal)
					text.assign(value.bstrVal);
				VariantClear(&value);
			}
			return text;
		}

		bool GetBoolProperty(IDispatch* object, const wchar_t* name)
		{
			VARIANT value;
			VariantInit(&value);
			bool result = false;
			if (GetPropertyVariant(object, name, &value))
			{
				if (value.vt == VT_BOOL)
					result = value.boolVal != VARIANT_FALSE;
				VariantClear(&value);
			}
			return result;
		}

		// 方法调用:参数按 IDispatch 约定**逆序**存放;这里只用到字符串参数。
		std::string Hex(HRESULT status)
		{
			char buffer[16] {};
			std::snprintf(buffer, sizeof(buffer), "%08lX", static_cast<unsigned long>(status));
			return std::string(buffer);
		}

		bool InvokeMethod(IDispatch* object, const wchar_t* name, const std::vector<std::wstring>& args,
			std::string* error)
		{
			if (!object)
			{
				if (error) *error = "no dispatch object";
				return false;
			}
			DISPID id = 0;
			LPOLESTR names[] = { const_cast<LPOLESTR>(name) };
			if (FAILED(object->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id)))
			{
				if (error) *error = "method not found: " + std::string(name, name + std::wcslen(name));
				return false;
			}
			std::vector<VARIANT> storage(args.size());
			for (size_t i = 0; i < args.size(); ++i)
			{
				VariantInit(&storage[i]);
				storage[i].vt = VT_BSTR;
				storage[i].bstrVal = ::SysAllocString(args[i].c_str());
			}
			std::vector<VARIANT> reversed(storage.rbegin(), storage.rend());
			DISPPARAMS parameters {};
			parameters.rgvarg = reversed.empty() ? nullptr : reversed.data();
			parameters.cArgs = static_cast<UINT>(reversed.size());
			parameters.cNamedArgs = 0;
			VARIANT result;
			VariantInit(&result);
			const HRESULT status = object->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
				DISPATCH_METHOD, &parameters, &result, nullptr, nullptr);
			VariantClear(&result);
			for (VARIANT& value : storage)
				VariantClear(&value);
			if (FAILED(status))
			{
				if (error) *error = "invoke failed (hr=0x" + Hex(status) + ")";
				return false;
			}
			return true;
		}

		// ---- moniker 名解析:`!VisualStudio.DTE.<major>.<minor>:<pid>` ----
		unsigned ParseDteMajor(const std::wstring& moniker)
		{
			const size_t marker = moniker.find(L".DTE.");
			if (marker == std::wstring::npos)
				return 0;
			const size_t begin = marker + 5;
			size_t end = begin;
			while (end < moniker.size() && moniker[end] >= L'0' && moniker[end] <= L'9')
				++end;
			return end > begin ? static_cast<unsigned>(std::wcstoul(moniker.substr(begin, end - begin).c_str(), nullptr, 10)) : 0;
		}

		unsigned long ParsePid(const std::wstring& moniker)
		{
			const size_t colon = moniker.rfind(L':');
			if (colon == std::wstring::npos || colon + 1 >= moniker.size())
				return 0;
			return std::wcstoul(moniker.c_str() + colon + 1, nullptr, 10);
		}

		std::wstring NormalizeRoot(const std::wstring& solutionFullName)
		{
			std::error_code error;
			const std::filesystem::path path(solutionFullName);
			if (std::filesystem::is_directory(path, error))
				return path.lexically_normal().wstring();
			return path.parent_path().lexically_normal().wstring();
		}

		bool ReadInstance(IUnknown* unknown, const std::wstring& moniker, RunningVisualStudio& out)
		{
			IDispatch* dte = nullptr;
			if (FAILED(unknown->QueryInterface(IID_IDispatch, reinterpret_cast<void**>(&dte))) || !dte)
				return false;
			bool ok = false;
			if (IDispatch* solution = GetDispatchProperty(dte, L"Solution"))
			{
				const std::wstring fullName = GetStringProperty(solution, L"FullName");
				const bool isOpen = GetBoolProperty(solution, L"IsOpen");
				if (isOpen && !fullName.empty())
				{
					const std::wstring root = NormalizeRoot(fullName);
					if (!root.empty())
					{
						out.Solution = fullName;
						out.Root = root;
						ok = true;
					}
				}
				solution->Release();
			}
			dte->Release();
			if (!ok)
				return false;
			out.Version = ParseDteMajor(moniker);
			out.Pid = ParsePid(moniker);
			return true;
		}

		void CollectRunningVisualStudio(std::vector<RunningVisualStudio>& out)
		{
			const char* traceEnv = std::getenv("WLD_VSOPEN_TRACE");
			const bool trace = traceEnv != nullptr && traceEnv[0] != '\0';
			auto Trace = [trace](const std::string& text)
			{
				if (trace)
					WLD_CORE_INFO("[vsopen-trace] {0}", text);
			};
			IRunningObjectTable* rot = nullptr;
			if (FAILED(::GetRunningObjectTable(0, &rot)) || !rot)
			{
				Trace("GetRunningObjectTable failed");
				return;
			}
			IEnumMoniker* monikers = nullptr;
			if (SUCCEEDED(rot->EnumRunning(&monikers)) && monikers)
			{
				// GetDisplayName 需要一个**真实**的 bind context:传 null 时 VS 的 moniker
				// 会直接失败(2026-09-30 实测:编辑器进程里 monikers=5 / visualStudio=0)。
				IBindCtx* bindContext = nullptr;
				::CreateBindCtx(0, &bindContext);
				size_t total = 0;
				size_t visualStudio = 0;
				IMoniker* moniker = nullptr;
				while (monikers->Next(1, &moniker, nullptr) == S_OK)
				{
					++total;
					LPOLESTR display = nullptr;
					const HRESULT nameStatus = moniker->GetDisplayName(bindContext, nullptr, &display);
					if (SUCCEEDED(nameStatus) && display)
					{
						const std::wstring name(display);
						::CoTaskMemFree(display);
						Trace("moniker[" + std::to_string(total) + "]=" + std::string(name.begin(), name.end()));
						if (name.rfind(L"!VisualStudio.DTE", 0) == 0)
						{
							++visualStudio;
							IUnknown* unknown = nullptr;
							if (SUCCEEDED(rot->GetObject(moniker, &unknown)) && unknown)
							{
								RunningVisualStudio instance;
								if (ReadInstance(unknown, name, instance))
									out.push_back(std::move(instance));
								else
									Trace("ReadInstance rejected the object");
								unknown->Release();
							}
							else
								Trace("GetObject failed");
						}
					}
					else
						Trace("GetDisplayName failed (hr=0x" + Hex(nameStatus) + ")");
					moniker->Release();
					moniker = nullptr;
				}
				Trace("monikers=" + std::to_string(total) + " visualStudio=" + std::to_string(visualStudio));
				if (bindContext)
					bindContext->Release();
				monikers->Release();
			}
			else
				Trace("EnumRunning failed");
			rot->Release();
		}

		// 统一分隔符后再做前缀比较:std::filesystem 的分量迭代会把 `E:/x` 与 `E:\x` 的
		// 根目录分量判成不同("/" vs "\\"),逐分量比较会整体失效(2026-09-30 实测踩到)。
		std::wstring NormalizeForCompare(const std::filesystem::path& path)
		{
			std::wstring text = path.lexically_normal().wstring();
			for (wchar_t& character : text)
				if (character == L'/')
					character = L'\\';
			while (text.size() > 1 && text.back() == L'\\')
				text.pop_back();
			return text;
		}

		bool PathHasPrefix(const std::filesystem::path& file, const std::filesystem::path& root)
		{
			const std::wstring fileText = NormalizeForCompare(file);
			const std::wstring rootText = NormalizeForCompare(root);
			if (rootText.empty() || fileText.size() < rootText.size())
				return false;
			if (_wcsnicmp(fileText.c_str(), rootText.c_str(), rootText.size()) != 0)
				return false;
			// 必须落在目录边界上:`E:\WorldEngine` 不能匹配 `E:\WorldEngine2\...`。
			return fileText.size() == rootText.size() || fileText[rootText.size()] == L'\\';
		}

		struct OpenOutcome
		{
			// 0 = 已在运行实例里打开;1 = 没找到该实例;2 = 找到但打开失败。
			int Status = 1;
			std::string Error;
		};

		// 跨线程只带可序列化标识(COM 对象不跨线程)。
		struct OpenRequest
		{
			std::wstring File;
			unsigned long Pid = 0;
			unsigned Version = 0;
		};

		void OpenInInstanceWork(const OpenRequest& request, OpenOutcome& outcome)
		{
			IRunningObjectTable* rot = nullptr;
			if (FAILED(::GetRunningObjectTable(0, &rot)) || !rot)
			{
				outcome.Status = 1;
				outcome.Error = "running object table unavailable";
				return;
			}
			IEnumMoniker* monikers = nullptr;
			if (FAILED(rot->EnumRunning(&monikers)) || !monikers)
			{
				rot->Release();
				outcome.Status = 1;
				outcome.Error = "cannot enumerate running objects";
				return;
			}
			IBindCtx* bindContext = nullptr;
			::CreateBindCtx(0, &bindContext);
			IMoniker* moniker = nullptr;
			while (monikers->Next(1, &moniker, nullptr) == S_OK)
			{
				LPOLESTR display = nullptr;
				if (SUCCEEDED(moniker->GetDisplayName(bindContext, nullptr, &display)) && display)
				{
					const std::wstring name(display);
					::CoTaskMemFree(display);
					if (name.rfind(L"!VisualStudio.DTE", 0) == 0
						&& ParsePid(name) == request.Pid && ParseDteMajor(name) == request.Version)
					{
						IUnknown* unknown = nullptr;
						if (SUCCEEDED(rot->GetObject(moniker, &unknown)) && unknown)
						{
							IDispatch* dte = nullptr;
							if (SUCCEEDED(unknown->QueryInterface(IID_IDispatch,
								reinterpret_cast<void**>(&dte))) && dte)
							{
								IDispatch* operations = GetDispatchProperty(dte, L"ItemOperations");
								std::string error;
								if (operations && InvokeMethod(operations, L"OpenFile",
									{ request.File, std::wstring() }, &error))
								{
									if (IDispatch* window = GetDispatchProperty(dte, L"MainWindow"))
									{
										std::string ignored;
										InvokeMethod(window, L"Activate", {}, &ignored);   // 尽力而为
										window->Release();
									}
									outcome.Status = 0;
								}
								else
								{
									outcome.Status = 2;
									outcome.Error = error.empty() ? "ItemOperations.OpenFile unavailable" : error;
								}
								if (operations)
									operations->Release();
								dte->Release();
							}
							unknown->Release();
						}
						moniker->Release();
						break;
					}
				}
				moniker->Release();
				moniker = nullptr;
			}
			if (bindContext)
				bindContext->Release();
			monikers->Release();
			rot->Release();
		}
	}

	std::vector<RunningVisualStudio> EnumerateRunningVisualStudio(unsigned timeoutMs)
	{
		std::vector<RunningVisualStudio> instances;
		RunComWorkWithTimeout<std::vector<RunningVisualStudio>>(
			[](std::vector<RunningVisualStudio>& out) { CollectRunningVisualStudio(out); },
			instances, timeoutMs);
		return instances;
	}

	bool FindRunningVisualStudioFor(const std::vector<RunningVisualStudio>& instances,
		const std::filesystem::path& absFile, RunningVisualStudio* out)
	{
		std::error_code error;
		const std::filesystem::path file = std::filesystem::absolute(absFile, error).lexically_normal();
		const RunningVisualStudio* best = nullptr;
		size_t bestDepth = 0;
		for (const RunningVisualStudio& instance : instances)
		{
			if (instance.Root.empty())
				continue;
			const std::filesystem::path root = std::filesystem::path(instance.Root).lexically_normal();
			if (!PathHasPrefix(file, root))
				continue;
			const size_t depth = static_cast<size_t>(std::distance(root.begin(), root.end()));
			if (!best || depth > bestDepth)
			{
				best = &instance;
				bestDepth = depth;
			}
		}
		if (!best)
			return false;
		if (out)
			*out = *best;
		return true;
	}

	bool OpenFileInRunningVisualStudio(const RunningVisualStudio& instance,
		const std::filesystem::path& absFile, std::string* error, unsigned timeoutMs)
	{
		// 只把可序列化的标识带进 worker(COM 对象本身不跨线程)。
		OpenRequest request;
		request.File = absFile.wstring();
		request.Pid = instance.Pid;
		request.Version = instance.Version;
		OpenOutcome outcome;
		const bool completed = RunComWorkWithTimeout<OpenOutcome>(
			[request](OpenOutcome& out)
			{
				OpenInInstanceWork(request, out);
			},
			outcome, timeoutMs);
		if (!completed)
		{
			if (error) *error = "timed out talking to Visual Studio";
			return false;
		}
		if (outcome.Status == 0)
			return true;
		if (error)
			*error = outcome.Error.empty()
				? (outcome.Status == 1 ? "the Visual Studio instance is gone" : "open failed")
				: outcome.Error;
		return false;
	}
}
