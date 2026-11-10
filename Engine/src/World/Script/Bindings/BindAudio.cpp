#include "wldpch.h"
#include "World/Script/Bindings/BindAudio.h"

#include "World/Audio/AudioSystem.h"
#include "World/Audio/AudioTypes.h"
#include "World/Audio/InteractiveMusic.h"
#include "World/Script/Bindings/BindServices.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Script/Vm/LuauVm.h"
#include "World/Script/Vm/ScriptBindingContext.h"
#include "World/Script/Vm/ScriptRef.h"
#include "World/Script/Vm/ScriptValue.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <cctype>
#include <unordered_map>
#include <vector>

namespace World
{
	namespace
	{
		std::mutex s_AiStimulusMutex;
		std::vector<AiStimulusRecord> s_RecentAiStimuli;
		constexpr std::size_t kMaxRecentAiStimuli = 128;
		std::mutex s_AudioParameterMutex;
		std::unordered_map<std::string, float> s_GlobalParameters;
		std::unordered_map<uint32_t, std::unordered_map<std::string, float>> s_VoiceParameters;
		const char* TypeNameOf(const ScriptValue& value)
		{
			return value.TypeName();
		}

		std::string MethodSignature(const std::string& table, const ScriptServiceMethod& method)
		{
			std::string signature = table + "." + method.Name + "(";
			for (std::size_t i = 0; i < method.ParamCount; ++i)
			{
				if (i) signature += ", ";
				signature += method.Params[i].Name;
				if (!method.Params[i].Required) signature += "?";
				signature += ": ";
				signature += method.Params[i].LuaType;
			}
			return signature + ")";
		}

		void NormalizeArgs(const ScriptValue*& args, std::size_t& count)
		{
			if (count > 0 && args[0].IsTable())
			{
				++args;
				--count;
			}
		}

		void CheckArgumentCount(const std::string& table, const ScriptServiceMethod& method,
			std::size_t count, std::size_t minimum)
		{
			if (count < minimum || count > method.ExpectedArgs)
				throw std::logic_error(MethodSignature(table, method) + " expects between " +
					std::to_string(minimum) + " and " + std::to_string(method.ExpectedArgs) +
					" argument(s); got " + std::to_string(count));
		}

		bool MatchesType(ScriptServiceArgType accepted, const ScriptValue& value)
		{
			if ((static_cast<uint32_t>(accepted) & static_cast<uint32_t>(ScriptServiceArgType::Boolean)) &&
				value.IsBoolean())
				return true;
			if ((static_cast<uint32_t>(accepted) & static_cast<uint32_t>(ScriptServiceArgType::Number)) &&
				value.IsNumber())
				return true;
			if ((static_cast<uint32_t>(accepted) & static_cast<uint32_t>(ScriptServiceArgType::String)) &&
				value.IsString())
				return true;
			if ((static_cast<uint32_t>(accepted) & static_cast<uint32_t>(ScriptServiceArgType::Table)) &&
				value.IsTable())
				return true;
			return false;
		}

		const ScriptValue& RequireArg(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index)
		{
			if (index >= argCount || args[index].IsNil())
				throw std::logic_error(MethodSignature(table, method) + " is missing required argument " +
					std::to_string(index + 1));
			if (!MatchesType(method.Params[index].Accepted, args[index]))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be " + method.Params[index].LuaType +
					", got " + TypeNameOf(args[index]));
			return args[index];
		}

		std::string RequireString(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index)
		{
			const ScriptValue& value = RequireArg(table, method, args, argCount, index);
			std::string result;
			if (!value.AsString(&result))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be " + method.Params[index].LuaType);
			return result;
		}

		double RequireNumber(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index)
		{
			const ScriptValue& value = RequireArg(table, method, args, argCount, index);
			double result = 0.0;
			if (!value.AsNumber(&result))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be " + method.Params[index].LuaType);
			return result;
		}

		double OptionalNumber(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index, double fallback)
		{
			if (index >= argCount || args[index].IsNil())
				return fallback;
			if (!MatchesType(method.Params[index].Accepted, args[index]))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be " + method.Params[index].LuaType +
					", got " + TypeNameOf(args[index]));
			double result = fallback;
			args[index].AsNumber(&result);
			return result;
		}

		bool RequireBool(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index)
		{
			const ScriptValue& value = RequireArg(table, method, args, argCount, index);
			bool result = false;
			if (!value.AsBool(&result))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be " + method.Params[index].LuaType);
			return result;
		}

		bool OptionalBool(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index, bool fallback)
		{
			if (index >= argCount || args[index].IsNil())
				return fallback;
			if (!MatchesType(method.Params[index].Accepted, args[index]))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be " + method.Params[index].LuaType +
					", got " + TypeNameOf(args[index]));
			bool result = fallback;
			args[index].AsBool(&result);
			return result;
		}

		std::string OptionalString(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index, const std::string& fallback)
		{
			if (index >= argCount || args[index].IsNil())
				return fallback;
			if (!MatchesType(method.Params[index].Accepted, args[index]))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be " + method.Params[index].LuaType +
					", got " + TypeNameOf(args[index]));
			std::string result = fallback;
			args[index].AsString(&result);
			return result;
		}

		// ---- 方法实现 ----

		ScriptValue AudioPostEventImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[0];
			CheckArgumentCount("Audio", method, count, 1);
			const std::string eventId = RequireString("Audio", method, args, count, 0);
			const double x = OptionalNumber("Audio", method, args, count, 1, 0.0);
			const double y = OptionalNumber("Audio", method, args, count, 2, 0.0);
			const double z = OptionalNumber("Audio", method, args, count, 3, 0.0);
			const double volume = OptionalNumber("Audio", method, args, count, 4, 1.0);
			const double pitch = OptionalNumber("Audio", method, args, count, 5, 1.0);
			const bool loop = OptionalBool("Audio", method, args, count, 6, false);

			Audio::VoiceHandle handle = Audio::AudioSystem::Get().PostEvent(
				eventId,
				glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)),
				static_cast<float>(volume),
				static_cast<float>(pitch),
				loop);
			return ScriptValue::Number(static_cast<double>(handle));
		}

		ScriptValue AudioPostEvent2DImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[1];
			CheckArgumentCount("Audio", method, count, 1);
			const std::string eventId = RequireString("Audio", method, args, count, 0);
			const double screenX = OptionalNumber("Audio", method, args, count, 1, 0.0);
			const double screenY = OptionalNumber("Audio", method, args, count, 2, 0.0);
			const double volume = OptionalNumber("Audio", method, args, count, 3, 1.0);
			const double pitch = OptionalNumber("Audio", method, args, count, 4, 1.0);
			const bool loop = OptionalBool("Audio", method, args, count, 5, false);

			Audio::VoiceHandle handle = Audio::AudioSystem::Get().PostEvent2D(
				eventId,
				glm::vec2(static_cast<float>(screenX), static_cast<float>(screenY)),
				static_cast<float>(volume),
				static_cast<float>(pitch),
				loop);
			return ScriptValue::Number(static_cast<double>(handle));
		}

		ScriptValue AudioPostComboImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[2];
			CheckArgumentCount("Audio", method, count, 1);
			const std::string eventId = RequireString("Audio", method, args, count, 0);
			const std::string comboGroup = OptionalString("Audio", method, args, count, 1, "default");
			const double screenX = OptionalNumber("Audio", method, args, count, 2, 0.0);
			const double screenY = OptionalNumber("Audio", method, args, count, 3, 0.0);
			const double volume = OptionalNumber("Audio", method, args, count, 4, 1.0);

			Audio::VoiceHandle handle = Audio::AudioSystem::Get().PostComboEvent(
				eventId,
				comboGroup,
				glm::vec2(static_cast<float>(screenX), static_cast<float>(screenY)),
				static_cast<float>(volume));
			return ScriptValue::Number(static_cast<double>(handle));
		}

		ScriptValue AudioStopImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[3];
			CheckArgumentCount("Audio", method, count, 1);
			const double handleNum = RequireNumber("Audio", method, args, count, 0);
			const double fade = OptionalNumber("Audio", method, args, count, 1, 0.05);

			Audio::AudioSystem::Get().StopVoice(
				static_cast<Audio::VoiceHandle>(handleNum),
				static_cast<float>(fade));
			return ScriptValue::Nil();
		}

		ScriptValue AudioPauseImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[4];
			CheckArgumentCount("Audio", method, count, 1);
			const double handleNum = RequireNumber("Audio", method, args, count, 0);

			Audio::AudioSystem::Get().PauseVoice(static_cast<Audio::VoiceHandle>(handleNum));
			return ScriptValue::Nil();
		}

		ScriptValue AudioResumeImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[5];
			CheckArgumentCount("Audio", method, count, 1);
			const double handleNum = RequireNumber("Audio", method, args, count, 0);

			Audio::AudioSystem::Get().ResumeVoice(static_cast<Audio::VoiceHandle>(handleNum));
			return ScriptValue::Nil();
		}

		ScriptValue AudioSetLoopingImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[6];
			CheckArgumentCount("Audio", method, count, 2);
			const double handleNum = RequireNumber("Audio", method, args, count, 0);
			const bool loop = RequireBool("Audio", method, args, count, 1);

			Audio::AudioSystem::Get().SetVoiceLooping(static_cast<Audio::VoiceHandle>(handleNum), loop);
			return ScriptValue::Nil();
		}

		ScriptValue AudioStopAllImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[7];
			CheckArgumentCount("Audio", method, count, 0);
			const double fade = OptionalNumber("Audio", method, args, count, 0, 0.05);

			Audio::AudioSystem::Get().StopAll(static_cast<float>(fade));
			return ScriptValue::Nil();
		}

		ScriptValue AudioSetBusVolumeImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[8];
			CheckArgumentCount("Audio", method, count, 2);
			const double busId = RequireNumber("Audio", method, args, count, 0);
			const double volume = RequireNumber("Audio", method, args, count, 1);

			Audio::AudioSystem::Get().SetBusVolume(
				static_cast<Audio::BusId>(busId),
				static_cast<float>(volume));
			return ScriptValue::Nil();
		}

		ScriptValue AudioTriggerSnapshotImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[9];
			CheckArgumentCount("Audio", method, count, 1);
			const std::string snapshotName = RequireString("Audio", method, args, count, 0);
			const double transition = OptionalNumber("Audio", method, args, count, 1, 0.3);

			Audio::AudioSystem::Get().TriggerSnapshot(snapshotName, static_cast<float>(transition));
			return ScriptValue::Nil();
		}

		ScriptValue AudioClearSnapshotImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[10];
			CheckArgumentCount("Audio", method, count, 0);
			const double transition = OptionalNumber("Audio", method, args, count, 0, 0.3);

			Audio::AudioSystem::Get().ClearSnapshot(static_cast<float>(transition));
			return ScriptValue::Nil();
		}

		ScriptValue AudioSetVoiceVolumetricRadiusImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[11];
			CheckArgumentCount("Audio", method, count, 2);
			const double handleNum = RequireNumber("Audio", method, args, count, 0);
			const double radius = RequireNumber("Audio", method, args, count, 1);

			Audio::AudioSystem::Get().SetVoiceVolumetricRadius(
				static_cast<Audio::VoiceHandle>(handleNum),
				static_cast<float>(radius));
			return ScriptValue::Nil();
		}

		ScriptValue AudioSetVoiceVelocityImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[12];
			CheckArgumentCount("Audio", method, count, 4);
			const double handleNum = RequireNumber("Audio", method, args, count, 0);
			const double vx = RequireNumber("Audio", method, args, count, 1);
			const double vy = RequireNumber("Audio", method, args, count, 2);
			const double vz = RequireNumber("Audio", method, args, count, 3);

			Audio::AudioSystem::Get().SetVoiceVelocity(
				static_cast<Audio::VoiceHandle>(handleNum),
				glm::vec3(static_cast<float>(vx), static_cast<float>(vy), static_cast<float>(vz)));
			return ScriptValue::Nil();
		}

		ScriptValue AudioSetVoiceOcclusionImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[13];
			CheckArgumentCount("Audio", method, count, 2);
			const double handleNum = RequireNumber("Audio", method, args, count, 0);
			const double occlusion = RequireNumber("Audio", method, args, count, 1);

			Audio::AudioSystem::Get().SetVoiceOcclusion(
				static_cast<Audio::VoiceHandle>(handleNum),
				static_cast<float>(occlusion));
			return ScriptValue::Nil();
		}

		ScriptValue AudioGetDspTimeImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[14];
			CheckArgumentCount("Audio", method, count, 0);

			double dspSeconds = Audio::AudioSystem::Get().GetDspTime();
			return ScriptValue::Number(dspSeconds);
		}

		ScriptValue AudioGetSpectrumImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[15];
			CheckArgumentCount("Audio", method, count, 0);
			const double bandCountDouble = OptionalNumber("Audio", method, args, count, 0, 64.0);
			uint32_t bandCount = static_cast<uint32_t>(std::clamp(static_cast<int>(bandCountDouble), 1, 512));

			ScriptTableRef table = ScriptEngine::GetState().CreateTable();
			if (!table.IsValid())
				return ScriptValue::Nil();

			std::vector<float> bands;
			Audio::AudioSystem::Get().GetSpectrum(bands, bandCount);
			if (bands.empty())
			{
				bands.assign(bandCount, 0.0f);
			}

			for (std::size_t i = 0; i < bands.size(); ++i)
			{
				table.SetArrayElement(i + 1, ScriptValue::Number(static_cast<double>(bands[i])));
			}

			return table.ToValue();
		}

		ScriptValue AudioIsBeatImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[16];
			CheckArgumentCount("Audio", method, count, 0);

			bool isBeat = Audio::AudioSystem::Get().IsBeatDetected() ||
				Audio::AudioSystem::Get().GetMusic().GetClock().DidCrossBeat();
			return ScriptValue::Boolean(isBeat);
		}

		ScriptValue AudioBroadcastAiStimulusImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[17];
			CheckArgumentCount("Audio", method, count, 6);
			const double x = RequireNumber("Audio", method, args, count, 0);
			const double y = RequireNumber("Audio", method, args, count, 1);
			const double z = RequireNumber("Audio", method, args, count, 2);
			const double radius = RequireNumber("Audio", method, args, count, 3);
			const double loudness = RequireNumber("Audio", method, args, count, 4);
			const std::string tag = RequireString("Audio", method, args, count, 5);

			Audio::SoundStimulus stimulus;
			stimulus.position = glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
			stimulus.radius = static_cast<float>(radius);
			stimulus.loudness = static_cast<float>(loudness);
			stimulus.tag = tag;
			Audio::AudioSystem::Get().BroadcastSoundStimulus(stimulus);

			AiStimulusRecord record;
			record.position = stimulus.position;
			record.radius = stimulus.radius;
			record.loudness = stimulus.loudness;
			record.tag = stimulus.tag;
			record.timestamp = Audio::AudioSystem::Get().GetDspTime();

			{
				std::lock_guard<std::mutex> lock(s_AiStimulusMutex);
				if (s_RecentAiStimuli.size() >= kMaxRecentAiStimuli)
				{
					s_RecentAiStimuli.erase(s_RecentAiStimuli.begin());
				}
				s_RecentAiStimuli.push_back(std::move(record));
			}

			return ScriptValue::Nil();
		}

		ScriptValue AudioGetProfilerStatsImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[18];
			CheckArgumentCount("Audio", method, count, 0);

			Audio::AudioProfilerStats snapshot = Audio::AudioSystem::Get().GetProfiler().GetSnapshot();
			double dspTime = snapshot.dspTimeSeconds > 0.0 ? snapshot.dspTimeSeconds : Audio::AudioSystem::Get().GetDspTime();

			ScriptTableRef stats = ScriptEngine::GetState().CreateTable();
			if (stats.IsValid())
			{
				stats.SetField("physical_voices", ScriptValue::Number(static_cast<double>(snapshot.activePhysicalVoices)));
				stats.SetField("virtual_voices", ScriptValue::Number(static_cast<double>(snapshot.activeVirtualVoices)));
				stats.SetField("clipping", ScriptValue::Boolean(snapshot.masterClipping));
				stats.SetField("dsp_time", ScriptValue::Number(dspTime));
				return stats.ToValue();
			}
			return ScriptValue::Nil();
		}

		ScriptValue AudioSetParameterImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[19];
			CheckArgumentCount("Audio", method, count, 2);
			const std::string name = RequireString("Audio", method, args, count, 0);
			const double value = RequireNumber("Audio", method, args, count, 1);

			SetAudioParameter(name, static_cast<float>(value));
			return ScriptValue::Nil();
		}

		ScriptValue AudioSetVoiceParameterImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[20];
			CheckArgumentCount("Audio", method, count, 3);
			const double voiceHandle = RequireNumber("Audio", method, args, count, 0);
			const std::string name = RequireString("Audio", method, args, count, 1);
			const double value = RequireNumber("Audio", method, args, count, 2);

			SetVoiceAudioParameter(static_cast<uint32_t>(voiceHandle), name, static_cast<float>(value));
			return ScriptValue::Nil();
		}

		ScriptValue AudioGetParameterImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[21];
			CheckArgumentCount("Audio", method, count, 1);
			const std::string name = RequireString("Audio", method, args, count, 0);

			float val = GetAudioParameter(name);
			return ScriptValue::Number(static_cast<double>(val));
		}

		ScriptValue AudioPlayFootstepImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[22];
			CheckArgumentCount("Audio", method, count, 1);
			const std::string surface = RequireString("Audio", method, args, count, 0);
			const double x = OptionalNumber("Audio", method, args, count, 1, 0.0);
			const double y = OptionalNumber("Audio", method, args, count, 2, 0.0);
			const double z = OptionalNumber("Audio", method, args, count, 3, 0.0);

			std::string lowerSurface = surface;
			std::transform(lowerSurface.begin(), lowerSurface.end(), lowerSurface.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });

			std::string eventId = "footstep_" + lowerSurface;
			auto& audioSystem = Audio::AudioSystem::Get();

			if (!audioSystem.FindSoundEvent(eventId) && !audioSystem.FindSoundEvent(surface))
			{
				if (const auto* baseEvt = audioSystem.FindSoundEvent("footstep_player"))
				{
					Audio::SoundEvent surfaceEvent(eventId, Audio::kBusSfx);
					surfaceEvent.GetSpatial().is3D = true;
					surfaceEvent.GetSpatial().minDistance = 1.0f;
					surfaceEvent.GetSpatial().maxDistance = 20.0f;
					if (lowerSurface == "stone" || lowerSurface == "concrete" || lowerSurface == "rock")
					{
						surfaceEvent.SetVolumeRange(0.9f, 1.1f);
						surfaceEvent.SetPitchRangeSemitones(1.0f, 3.0f);
					}
					else if (lowerSurface == "wood")
					{
						surfaceEvent.SetVolumeRange(0.85f, 1.05f);
						surfaceEvent.SetPitchRangeSemitones(-1.0f, 1.0f);
					}
					else if (lowerSurface == "metal")
					{
						surfaceEvent.SetVolumeRange(0.9f, 1.1f);
						surfaceEvent.SetPitchRangeSemitones(3.0f, 5.0f);
					}
					else
					{
						surfaceEvent.SetVolumeRange(0.8f, 1.0f);
						surfaceEvent.SetPitchRangeSemitones(-1.5f, 1.5f);
					}

					if (baseEvt->GetRootContainer() && !baseEvt->GetRootContainer()->variations.empty())
					{
						surfaceEvent.GetRootContainer()->variations = baseEvt->GetRootContainer()->variations;
					}
					else
					{
						surfaceEvent.GetRootContainer()->variations.push_back({ "assets/audio/sfx/footstep_grass.waudio", 1.0f });
					}
					audioSystem.RegisterSoundEvent(surfaceEvent);
				}
				else
				{
					eventId = "footstep_player";
				}
			}

			const std::string chosenEvent = audioSystem.FindSoundEvent(eventId) ? eventId :
				(audioSystem.FindSoundEvent(surface) ? surface : "footstep_player");

			Audio::VoiceHandle handle = audioSystem.PostEvent(
				chosenEvent,
				glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)));
			return ScriptValue::Number(static_cast<double>(handle));
		}

		ScriptValue MusicPlayImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[1].Methods[0];
			CheckArgumentCount("Music", method, count, 1);
			const std::string trackId = RequireString("Music", method, args, count, 0);
			const double fade = OptionalNumber("Music", method, args, count, 1, 0.5);

			Audio::AudioSystem::Get().GetMusic().Play(trackId, static_cast<float>(fade));
			return ScriptValue::Nil();
		}

		ScriptValue MusicTransitionToImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[1].Methods[1];
			CheckArgumentCount("Music", method, count, 1);
			const std::string trackId = RequireString("Music", method, args, count, 0);
			const double sync = OptionalNumber("Music", method, args, count, 1, 2.0);
			const double fade = OptionalNumber("Music", method, args, count, 2, 1.0);

			Audio::TransitionSync syncMode = Audio::TransitionSync::NextBar;
			int syncInt = static_cast<int>(sync);
			if (syncInt == 0) syncMode = Audio::TransitionSync::Immediate;
			else if (syncInt == 1) syncMode = Audio::TransitionSync::NextBeat;
			else syncMode = Audio::TransitionSync::NextBar;

			Audio::AudioSystem::Get().GetMusic().TransitionTo(trackId, syncMode, static_cast<float>(fade));
			return ScriptValue::Nil();
		}

		ScriptValue MusicStopImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[1].Methods[2];
			CheckArgumentCount("Music", method, count, 0);
			const double fade = OptionalNumber("Music", method, args, count, 0, 1.0);

			Audio::AudioSystem::Get().GetMusic().Stop(static_cast<float>(fade));
			return ScriptValue::Nil();
		}

		ScriptValue AudioSetPausedImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[23];
			CheckArgumentCount("Audio", method, count, 1);
			const bool paused = RequireBool("Audio", method, args, count, 0);
			Audio::AudioSystem::Get().SetPaused(paused);
			return ScriptValue::Nil();
		}

		ScriptValue AudioIsPausedImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[24];
			CheckArgumentCount("Audio", method, count, 0);
			return ScriptValue::Boolean(Audio::AudioSystem::Get().IsPaused());
		}

		ScriptValue AudioSetTimeDilationImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[25];
			CheckArgumentCount("Audio", method, count, 1);
			const double timeScale = RequireNumber("Audio", method, args, count, 0);
			const double pitchScale = OptionalNumber("Audio", method, args, count, 1, -1.0);
			Audio::AudioSystem::Get().SetTimeDilation(static_cast<float>(timeScale), static_cast<float>(pitchScale));
			return ScriptValue::Nil();
		}

		ScriptValue AudioPlayTileFootstepImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[26];
			CheckArgumentCount("Audio", method, count, 3);
			const double tileX = RequireNumber("Audio", method, args, count, 0);
			const double tileY = RequireNumber("Audio", method, args, count, 1);
			const std::string surface = RequireString("Audio", method, args, count, 2);
			const double worldX = OptionalNumber("Audio", method, args, count, 3, 0.0);
			const double worldY = OptionalNumber("Audio", method, args, count, 4, 0.0);
			Audio::VoiceHandle h = Audio::AudioSystem::Get().PlayTileFootstep(
				static_cast<int>(tileX), static_cast<int>(tileY), surface,
				glm::vec2(static_cast<float>(worldX), static_cast<float>(worldY)));
			return ScriptValue::Number(static_cast<double>(h));
		}

		ScriptValue AudioSetVoiceVolumeImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[27];
			CheckArgumentCount("Audio", method, count, 2);
			const double handle = RequireNumber("Audio", method, args, count, 0);
			const double vol = RequireNumber("Audio", method, args, count, 1);
			Audio::AudioSystem::Get().SetVoiceVolume(static_cast<Audio::VoiceHandle>(handle), static_cast<float>(vol));
			return ScriptValue::Nil();
		}

		ScriptValue AudioSetVoicePitchImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[28];
			CheckArgumentCount("Audio", method, count, 2);
			const double handle = RequireNumber("Audio", method, args, count, 0);
			const double pitch = RequireNumber("Audio", method, args, count, 1);
			Audio::AudioSystem::Get().SetVoicePitch(static_cast<Audio::VoiceHandle>(handle), static_cast<float>(pitch));
			return ScriptValue::Nil();
		}

		ScriptValue MusicSetStemVolumeImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[1].Methods[3];
			CheckArgumentCount("Music", method, count, 2);
			const std::string stem = RequireString("Music", method, args, count, 0);
			const double vol = RequireNumber("Music", method, args, count, 1);
			const double fade = OptionalNumber("Music", method, args, count, 2, 0.5);
			Audio::AudioSystem::Get().GetMusic().SetStemVolume(stem, static_cast<float>(vol), static_cast<float>(fade));
			return ScriptValue::Nil();
		}

		ScriptValue MusicGetStemVolumeImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[1].Methods[4];
			CheckArgumentCount("Music", method, count, 1);
			const std::string stem = RequireString("Music", method, args, count, 0);
			float v = Audio::AudioSystem::Get().GetMusic().GetStemVolume(stem);
			return ScriptValue::Number(static_cast<double>(v));
		}

		ScriptValue MusicPlayStingerImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[1].Methods[5];
			CheckArgumentCount("Music", method, count, 1);
			const std::string stinger = RequireString("Music", method, args, count, 0);
			const double vol = OptionalNumber("Music", method, args, count, 1, 1.0);
			Audio::VoiceHandle h = Audio::AudioSystem::Get().GetMusic().PlayStinger(stinger, static_cast<float>(vol));
			return ScriptValue::Number(static_cast<double>(h));
		}

		ScriptValue MusicSetAudioOffsetMsImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[1].Methods[6];
			CheckArgumentCount("Music", method, count, 1);
			const double ms = RequireNumber("Music", method, args, count, 0);
			Audio::AudioSystem::Get().GetMusic().SetAudioOffsetMs(static_cast<float>(ms));
			return ScriptValue::Nil();
		}

		ScriptValue MusicGetAudioOffsetMsImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[1].Methods[7];
			CheckArgumentCount("Music", method, count, 0);
			return ScriptValue::Number(static_cast<double>(Audio::AudioSystem::Get().GetMusic().GetAudioOffsetMs()));
		}

		// ---- 描述表 ----

		static const ScriptServiceParam audioPostEventParams[] = {
			{ "eventId", "string", ScriptServiceArgType::String, true, "Sound event identifier." },
			{ "x", "number", ScriptServiceArgType::Number, false, "World X position (default 0)." },
			{ "y", "number", ScriptServiceArgType::Number, false, "World Y position (default 0)." },
			{ "z", "number", ScriptServiceArgType::Number, false, "World Z position (default 0)." },
			{ "volume", "number", ScriptServiceArgType::Number, false, "Volume scale multiplier (default 1.0)." },
			{ "pitch", "number", ScriptServiceArgType::Number, false, "Pitch scale multiplier (default 1.0)." },
			{ "loop", "boolean", ScriptServiceArgType::Boolean, false, "Whether playback loops (default false)." },
		};

		static const ScriptServiceParam audioPostEvent2DParams[] = {
			{ "eventId", "string", ScriptServiceArgType::String, true, "Sound event identifier." },
			{ "screenX", "number", ScriptServiceArgType::Number, false, "Screen or viewport X coordinate (default 0)." },
			{ "screenY", "number", ScriptServiceArgType::Number, false, "Screen or viewport Y coordinate (default 0)." },
			{ "volume", "number", ScriptServiceArgType::Number, false, "Volume scale multiplier (default 1.0)." },
			{ "pitch", "number", ScriptServiceArgType::Number, false, "Pitch scale multiplier (default 1.0)." },
			{ "loop", "boolean", ScriptServiceArgType::Boolean, false, "Whether playback loops (default false)." },
		};

		static const ScriptServiceParam audioPostComboParams[] = {
			{ "eventId", "string", ScriptServiceArgType::String, true, "Sound event identifier." },
			{ "comboGroup", "string", ScriptServiceArgType::String, false, "Combo escalation group name (default 'default')." },
			{ "screenX", "number", ScriptServiceArgType::Number, false, "Screen or viewport X coordinate (default 0)." },
			{ "screenY", "number", ScriptServiceArgType::Number, false, "Screen or viewport Y coordinate (default 0)." },
			{ "volume", "number", ScriptServiceArgType::Number, false, "Volume scale multiplier (default 1.0)." },
		};

		static const ScriptServiceParam audioStopParams[] = {
			{ "voiceHandle", "number", ScriptServiceArgType::Number, true, "Voice handle returned by PostEvent." },
			{ "fadeSeconds", "number", ScriptServiceArgType::Number, false, "Fade-out duration in seconds (default 0.05)." },
		};

		static const ScriptServiceParam audioVoiceParams[] = {
			{ "voiceHandle", "number", ScriptServiceArgType::Number, true, "Voice handle returned by PostEvent." },
		};

		static const ScriptServiceParam audioSetLoopingParams[] = {
			{ "voiceHandle", "number", ScriptServiceArgType::Number, true, "Voice handle returned by PostEvent." },
			{ "loop", "boolean", ScriptServiceArgType::Boolean, true, "Whether playback loops." },
		};

		static const ScriptServiceParam audioFadeOnlyParams[] = {
			{ "fadeSeconds", "number", ScriptServiceArgType::Number, false, "Fade-out duration in seconds (default 0.05)." },
		};

		static const ScriptServiceParam audioSetBusVolumeParams[] = {
			{ "busId", "number", ScriptServiceArgType::Number, true, "Mixer bus id (0=Master, 1=Music, 2=Sfx, 3=Voice, 4=UI, 5=Ambience)." },
			{ "volume", "number", ScriptServiceArgType::Number, true, "Linear volume scale." },
		};

		static const ScriptServiceParam audioTriggerSnapshotParams[] = {
			{ "snapshotName", "string", ScriptServiceArgType::String, true, "Mixer snapshot preset name." },
			{ "transitionSeconds", "number", ScriptServiceArgType::Number, false, "Transition duration in seconds (default 0.3)." },
		};

		static const ScriptServiceParam audioTransitionOnlyParams[] = {
			{ "transitionSeconds", "number", ScriptServiceArgType::Number, false, "Transition duration in seconds (default 0.3)." },
		};

		
		static const ScriptServiceParam audioSetVoiceVolumetricRadiusParams[] = {
			{ "voiceHandle", "number", ScriptServiceArgType::Number, true, "Voice handle returned by PostEvent." },
			{ "radius", "number", ScriptServiceArgType::Number, true, "Near-field volumetric spread radius." },
		};

		static const ScriptServiceParam audioSetVoiceVelocityParams[] = {
			{ "voiceHandle", "number", ScriptServiceArgType::Number, true, "Voice handle returned by PostEvent." },
			{ "vx", "number", ScriptServiceArgType::Number, true, "Velocity X component in world units per second." },
			{ "vy", "number", ScriptServiceArgType::Number, true, "Velocity Y component in world units per second." },
			{ "vz", "number", ScriptServiceArgType::Number, true, "Velocity Z component in world units per second." },
		};

		static const ScriptServiceParam audioSetVoiceOcclusionParams[] = {
			{ "voiceHandle", "number", ScriptServiceArgType::Number, true, "Voice handle returned by PostEvent." },
			{ "occlusion", "number", ScriptServiceArgType::Number, true, "Direct occlusion factor between 0.0 (unoccluded) and 1.0 (fully occluded)." },
		};

		static const ScriptServiceParam audioGetSpectrumParams[] = {
			{ "bandCount", "number", ScriptServiceArgType::Number, false, "Number of frequency bands (default 64)." },
		};

		static const ScriptServiceParam audioBroadcastAiStimulusParams[] = {
			{ "x", "number", ScriptServiceArgType::Number, true, "World X position." },
			{ "y", "number", ScriptServiceArgType::Number, true, "World Y position." },
			{ "z", "number", ScriptServiceArgType::Number, true, "World Z position." },
			{ "radius", "number", ScriptServiceArgType::Number, true, "Propagation radius in world units." },
			{ "loudness", "number", ScriptServiceArgType::Number, true, "Loudness factor between 0.0 and 1.0." },
			{ "tag", "string", ScriptServiceArgType::String, true, "Semantic stimulus tag." },
		};

		static const ScriptServiceParam audioSetParameterParams[] = {
			{ "name", "string", ScriptServiceArgType::String, true, "RTPC parameter name." },
			{ "value", "number", ScriptServiceArgType::Number, true, "RTPC parameter value." },
		};

		static const ScriptServiceParam audioSetVoiceParameterParams[] = {
			{ "voiceHandle", "number", ScriptServiceArgType::Number, true, "Voice handle returned by PostEvent." },
			{ "name", "string", ScriptServiceArgType::String, true, "Local RTPC parameter name." },
			{ "value", "number", ScriptServiceArgType::Number, true, "Local RTPC parameter value." },
		};

		static const ScriptServiceParam audioGetParameterParams[] = {
			{ "name", "string", ScriptServiceArgType::String, true, "RTPC parameter name." },
		};

		static const ScriptServiceParam audioPlayFootstepParams[] = {
			{ "surface", "string", ScriptServiceArgType::String, true, "Surface material name ('Grass', 'Stone', 'Wood', etc.)." },
			{ "x", "number", ScriptServiceArgType::Number, false, "World X position (default 0)." },
			{ "y", "number", ScriptServiceArgType::Number, false, "World Y position (default 0)." },
			{ "z", "number", ScriptServiceArgType::Number, false, "World Z position (default 0)." },
		};
		static const ScriptServiceParam audioSetPausedParams[] = {
			{ "paused", "boolean", ScriptServiceArgType::Boolean, true, "Whether to pause gameplay bus audio." },
		};

		static const ScriptServiceParam audioSetTimeDilationParams[] = {
			{ "timeScale", "number", ScriptServiceArgType::Number, true, "Gameplay time scale factor (0.05 to 4.0)." },
			{ "pitchScale", "number", ScriptServiceArgType::Number, false, "Optional pitch scale override (default matches timeScale)." },
		};

		static const ScriptServiceParam audioPlayTileFootstepParams[] = {
			{ "tileX", "number", ScriptServiceArgType::Number, true, "Tilemap X grid coordinate." },
			{ "tileY", "number", ScriptServiceArgType::Number, true, "Tilemap Y grid coordinate." },
			{ "surface", "string", ScriptServiceArgType::String, true, "Surface material name ('Grass', 'Stone', 'Wood', etc.)." },
			{ "worldX", "number", ScriptServiceArgType::Number, false, "World X position (default 0)." },
			{ "worldY", "number", ScriptServiceArgType::Number, false, "World Y position (default 0)." },
		};

		static const ScriptServiceParam audioSetVoiceVolumeParams[] = {
			{ "voiceHandle", "number", ScriptServiceArgType::Number, true, "Voice handle returned by PostEvent." },
			{ "volume", "number", ScriptServiceArgType::Number, true, "Volume scale multiplier." },
		};

		static const ScriptServiceParam audioSetVoicePitchParams[] = {
			{ "voiceHandle", "number", ScriptServiceArgType::Number, true, "Voice handle returned by PostEvent." },
			{ "pitch", "number", ScriptServiceArgType::Number, true, "Pitch scale multiplier." },
		};

		static const ScriptServiceParam musicSetStemVolumeParams[] = {
			{ "stemName", "string", ScriptServiceArgType::String, true, "Music stem identifier ('Percussion', 'Bass', etc.)." },
			{ "volume", "number", ScriptServiceArgType::Number, true, "Target stem volume (0.0 to 4.0)." },
			{ "fadeSeconds", "number", ScriptServiceArgType::Number, false, "Crossfade duration in seconds (default 0.5)." },
		};

		static const ScriptServiceParam musicGetStemVolumeParams[] = {
			{ "stemName", "string", ScriptServiceArgType::String, true, "Music stem identifier." },
		};

		static const ScriptServiceParam musicPlayStingerParams[] = {
			{ "stingerEvent", "string", ScriptServiceArgType::String, true, "Stinger sound event name." },
			{ "volume", "number", ScriptServiceArgType::Number, false, "Volume multiplier (default 1.0)." },
		};

		static const ScriptServiceParam musicSetAudioOffsetMsParams[] = {
			{ "offsetMs", "number", ScriptServiceArgType::Number, true, "Audio offset calibration in milliseconds." },
		};

		static const ScriptServiceMethod audioMethods[] = {
			{ "PostEvent", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioPostEventImpl(args, count);
			}, audioPostEventParams, 7, 7, "number",
				"Post a 3D sound event at world position; returns a voice handle or 0 on failure." },

			{ "PostEvent2D", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioPostEvent2DImpl(args, count);
			}, audioPostEvent2DParams, 6, 6, "number",
				"Post a 2D sound event with screen panning and attenuation; returns a voice handle or 0 on failure." },

			{ "PostCombo", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioPostComboImpl(args, count);
			}, audioPostComboParams, 5, 5, "number",
				"Post a combo sound event with automatic pitch escalation; returns a voice handle or 0 on failure." },

			{ "Stop", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioStopImpl(args, count);
			}, audioStopParams, 2, 2, "",
				"Stop an active voice with optional fade-out duration." },

			{ "Pause", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioPauseImpl(args, count);
			}, audioVoiceParams, 1, 1, "",
				"Pause playback of an active voice." },

			{ "Resume", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioResumeImpl(args, count);
			}, audioVoiceParams, 1, 1, "",
				"Resume playback of a paused voice." },

			{ "SetLooping", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioSetLoopingImpl(args, count);
			}, audioSetLoopingParams, 2, 2, "",
				"Set looping state for an active voice." },

			{ "StopAll", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioStopAllImpl(args, count);
			}, audioFadeOnlyParams, 1, 1, "",
				"Stop all active voices with optional fade-out duration." },

			{ "SetBusVolume", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioSetBusVolumeImpl(args, count);
			}, audioSetBusVolumeParams, 2, 2, "",
				"Set mixer bus volume." },

			{ "TriggerSnapshot", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioTriggerSnapshotImpl(args, count);
			}, audioTriggerSnapshotParams, 2, 2, "",
				"Activate a mixer snapshot with optional transition duration." },

			{ "ClearSnapshot", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioClearSnapshotImpl(args, count);
			}, audioTransitionOnlyParams, 1, 1, "",
				"Clear active mixer snapshot with optional transition duration." },
			{ "SetVoiceVolumetricRadius", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioSetVoiceVolumetricRadiusImpl(args, count);
			}, audioSetVoiceVolumetricRadiusParams, 2, 2, "",
				"Set near-field volumetric spread radius for an active voice." },

			{ "SetVoiceVelocity", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioSetVoiceVelocityImpl(args, count);
			}, audioSetVoiceVelocityParams, 4, 4, "",
				"Set velocity vector for an active voice to drive Doppler pitch shift." },

			{ "SetVoiceOcclusion", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioSetVoiceOcclusionImpl(args, count);
			}, audioSetVoiceOcclusionParams, 2, 2, "",
				"Set direct occlusion factor for an active voice." },

			{ "GetDspTime", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioGetDspTimeImpl(args, count);
			}, nullptr, 0, 0, "number",
				"Get current DSP audio clock time in seconds based on cumulative hardware mix frames." },

			{ "GetSpectrum", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioGetSpectrumImpl(args, count);
			}, audioGetSpectrumParams, 1, 1, "table",
				"Get normalized FFT spectrum energy array for master mix." },

			{ "IsBeat", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioIsBeatImpl(args, count);
			}, nullptr, 0, 0, "boolean",
				"Check if current frame hits a low-frequency rhythm beat pulse." },

			{ "BroadcastAiStimulus", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioBroadcastAiStimulusImpl(args, count);
			}, audioBroadcastAiStimulusParams, 6, 6, "",
				"Broadcast an acoustic stimulus event to the AI perception system." },

			{ "GetProfilerStats", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioGetProfilerStatsImpl(args, count);
			}, nullptr, 0, 0, "table",
				"Get real-time audio profiler metrics including active voice counts, clipping, and DSP time." },

			{ "SetParameter", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioSetParameterImpl(args, count);
			}, audioSetParameterParams, 2, 2, "",
				"Set a global RTPC parameter value to drive sound curves and mixer state." },

			{ "SetVoiceParameter", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioSetVoiceParameterImpl(args, count);
			}, audioSetVoiceParameterParams, 3, 3, "",
				"Set a local RTPC parameter value on a specific voice instance." },

			{ "GetParameter", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioGetParameterImpl(args, count);
			}, audioGetParameterParams, 1, 1, "number",
				"Get current global RTPC parameter value." },

			{ "PlayFootstep", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioPlayFootstepImpl(args, count);
			}, audioPlayFootstepParams, 4, 4, "number",
				"Play a surface-specific footstep sound event; returns a voice handle or 0 on failure." },

			{ "SetPaused", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioSetPausedImpl(args, count);
			}, audioSetPausedParams, 1, 1, "",
				"Pause or unpause gameplay audio buses while keeping UI and music playing." },

			{ "IsPaused", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioIsPausedImpl(args, count);
			}, nullptr, 0, 0, "boolean",
				"Check if gameplay audio buses are currently paused." },

			{ "SetTimeDilation", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioSetTimeDilationImpl(args, count);
			}, audioSetTimeDilationParams, 2, 2, "",
				"Set gameplay time scale and pitch dilation factor (e.g. bullet time)." },

			{ "PlayTileFootstep", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioPlayTileFootstepImpl(args, count);
			}, audioPlayTileFootstepParams, 5, 5, "number",
				"Play a tilemap surface-specific footstep event; returns a voice handle or 0 on failure." },

			{ "SetVoiceVolume", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioSetVoiceVolumeImpl(args, count);
			}, audioSetVoiceVolumeParams, 2, 2, "",
				"Set volume multiplier for an active voice instance." },

			{ "SetVoicePitch", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioSetVoicePitchImpl(args, count);
			}, audioSetVoicePitchParams, 2, 2, "",
				"Set pitch multiplier for an active voice instance." },
		};

		static const ScriptServiceParam musicPlayParams[] = {
			{ "trackId", "string", ScriptServiceArgType::String, true, "Music track identifier." },
			{ "fadeSeconds", "number", ScriptServiceArgType::Number, false, "Fade-in duration in seconds (default 0.5)." },
		};

		static const ScriptServiceParam musicTransitionToParams[] = {
			{ "trackId", "string", ScriptServiceArgType::String, true, "Target music track identifier." },
			{ "sync", "number", ScriptServiceArgType::Number, false, "Sync mode: 0=Immediate, 1=NextBeat, 2=NextBar (default 2)." },
			{ "fadeSeconds", "number", ScriptServiceArgType::Number, false, "Crossfade duration in seconds (default 1.0)." },
		};

		static const ScriptServiceParam musicStopParams[] = {
			{ "fadeSeconds", "number", ScriptServiceArgType::Number, false, "Fade-out duration in seconds (default 1.0)." },
		};

		static const ScriptServiceMethod musicMethods[] = {
			{ "Play", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return MusicPlayImpl(args, count);
			}, musicPlayParams, 2, 2, "",
				"Play a registered music track with optional fade-in duration." },

			{ "TransitionTo", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return MusicTransitionToImpl(args, count);
			}, musicTransitionToParams, 3, 3, "",
				"Transition to another music track with beat/bar synchronization." },

			{ "Stop", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return MusicStopImpl(args, count);
			}, musicStopParams, 1, 1, "",
				"Stop active music playback with optional fade-out duration." },
		};

		static const ScriptServiceBinding audioServiceBindingsTable[] = {
			{
				"Audio",
				"Read-only audio service table for sound events, playback control, bus volumes, and snapshots.",
				audioMethods,
				sizeof(audioMethods) / sizeof(audioMethods[0]),
			},
			{
				"Music",
				"Read-only interactive music service table for playback and transitions.",
				musicMethods,
				sizeof(musicMethods) / sizeof(musicMethods[0]),
			},
		};

		bool RegisterServiceTable(ScriptBindingContext& bindings, const ScriptServiceBinding& service,
			std::string* error)
		{
			LuauVm* vm = bindings.Vm();
			if (!vm || !service.Name || !service.Name[0] || !service.Methods || service.MethodCount == 0)
			{
				if (error) *error = "invalid service binding descriptor";
				return false;
			}
			if (vm->GetGlobal(service.Name).Type() != ScriptValueType::Nil)
			{
				if (error) *error = std::string("global name is already in use: ") + service.Name;
				return false;
			}
			ScriptTableRef table = vm->CreateTable();
			if (!table.IsValid())
			{
				if (error) *error = std::string("failed to create the service table: ") + service.Name;
				return false;
			}
			for (std::size_t index = 0; index < service.MethodCount; ++index)
			{
				const ScriptServiceMethod& method = service.Methods[index];
				if (!method.Name || !method.Name[0] || !method.Function)
				{
					if (error) *error = std::string("service '") + service.Name + "' has an invalid method entry";
					return false;
				}
				std::size_t required = 0;
				for (std::size_t param = 0; param < method.ParamCount; ++param)
					if (method.Params[param].Required) ++required;
				if (required > method.ExpectedArgs || method.ExpectedArgs > method.ParamCount)
				{
					if (error) *error = std::string("service '") + service.Name + "." + method.Name +
						"' has an inconsistent parameter descriptor";
					return false;
				}
				if (!table.SetField(method.Name, bindings.CreateFunction(method.Name, method.Function)))
				{
					if (error) *error = std::string("failed to bind ") + service.Name + "." + method.Name;
					return false;
				}
			}
			if (!table.SetMetaField("__newindex", bindings.CreateFunction(service.Name,
				[](const ScriptValue*, std::size_t) -> ScriptValue
				{
					throw std::logic_error("service tables are read-only; use the documented methods");
				})))
			{
				if (error) *error = std::string("failed to lock the service table: ") + service.Name;
				return false;
			}
			if (!vm->SetGlobal(service.Name, table.ToValue()))
			{
				if (error) *error = std::string("failed to publish the service table: ") + service.Name;
				return false;
			}
			return true;
		}
	}

	bool RegisterAudioBindings(ScriptBindingContext& bindings, std::string* error)
	{
		std::size_t count = 0;
		const ScriptServiceBinding* services = AudioServiceBindings(&count);
		for (std::size_t index = 0; index < count; ++index)
		{
			if (!RegisterServiceTable(bindings, services[index], error))
				return false;
		}
		if (error) error->clear();
		return true;
	}

	const ScriptServiceBinding* AudioServiceBindings(std::size_t* count)
	{
		if (count)
			*count = sizeof(audioServiceBindingsTable) / sizeof(audioServiceBindingsTable[0]);
		return audioServiceBindingsTable;
	}

	std::vector<AiStimulusRecord> GetRecentAiStimuli()
	{
		std::lock_guard<std::mutex> lock(s_AiStimulusMutex);
		return s_RecentAiStimuli;
	}

	void ClearRecentAiStimuli()
	{
		std::lock_guard<std::mutex> lock(s_AiStimulusMutex);
		s_RecentAiStimuli.clear();
	}

	void SetAudioParameter(const std::string& name, float value)
	{
		{
			std::lock_guard<std::mutex> lock(s_AudioParameterMutex);
			s_GlobalParameters[name] = value;
		}

		if (name == "MasterVolume" || name == "master_volume")
		{
			Audio::AudioSystem::Get().SetBusVolume(Audio::kBusMaster, value);
		}
		else if (name == "MusicVolume" || name == "music_volume")
		{
			Audio::AudioSystem::Get().SetBusVolume(Audio::kBusMusic, value);
		}
		else if (name == "SfxVolume" || name == "sfx_volume")
		{
			Audio::AudioSystem::Get().SetBusVolume(Audio::kBusSfx, value);
		}
		else if (name == "VoiceVolume" || name == "voice_volume")
		{
			Audio::AudioSystem::Get().SetBusVolume(Audio::kBusVoice, value);
		}
		else if (name == "AmbienceVolume" || name == "ambience_volume")
		{
			Audio::AudioSystem::Get().SetBusVolume(Audio::kBusAmbience, value);
		}
	}

	float GetAudioParameter(const std::string& name)
	{
		std::lock_guard<std::mutex> lock(s_AudioParameterMutex);
		auto it = s_GlobalParameters.find(name);
		if (it != s_GlobalParameters.end())
			return it->second;
		return 0.0f;
	}

	void SetVoiceAudioParameter(uint32_t voiceHandle, const std::string& name, float value)
	{
		{
			std::lock_guard<std::mutex> lock(s_AudioParameterMutex);
			s_VoiceParameters[voiceHandle][name] = value;
		}

		auto* voice = Audio::AudioSystem::Get().GetVoicePool().FindVoice(static_cast<Audio::VoiceHandle>(voiceHandle));
		if (voice && voice->IsActive())
		{
			if (name == "Volume" || name == "volume" || name == "gain")
			{
				voice->SetVolume(value);
			}
			else if (name == "Pitch" || name == "pitch")
			{
				voice->SetPitch(value);
			}
			else if (name == "Occlusion" || name == "occlusion")
			{
				Audio::AudioSystem::Get().SetVoiceOcclusion(static_cast<Audio::VoiceHandle>(voiceHandle), value);
			}
			else if (name == "VolumetricRadius" || name == "volumetric_radius" || name == "radius")
			{
				Audio::AudioSystem::Get().SetVoiceVolumetricRadius(static_cast<Audio::VoiceHandle>(voiceHandle), value);
			}
			else if (name == "Looping" || name == "looping" || name == "loop")
			{
				Audio::AudioSystem::Get().SetVoiceLooping(static_cast<Audio::VoiceHandle>(voiceHandle), value > 0.5f);
			}
			else if (name == "Bitcrusher" || name == "bitcrusher")
			{
				voice->SetBitcrusher(value > 0.5f);
			}
		}
	}

	float GetVoiceAudioParameter(uint32_t voiceHandle, const std::string& name)
	{
		std::lock_guard<std::mutex> lock(s_AudioParameterMutex);
		auto vit = s_VoiceParameters.find(voiceHandle);
		if (vit != s_VoiceParameters.end())
		{
			auto pit = vit->second.find(name);
			if (pit != vit->second.end())
				return pit->second;
		}
		return 0.0f;
	}

	void ClearAudioParameters()
	{
		std::lock_guard<std::mutex> lock(s_AudioParameterMutex);
		s_GlobalParameters.clear();
		s_VoiceParameters.clear();
	}
}
