#include "wldpch.h"
#include "World/Script/Bindings/BindAudio.h"

#include "World/Audio/AudioSystem.h"
#include "World/Audio/AudioTypes.h"
#include "World/Audio/InteractiveMusic.h"
#include "World/Script/Bindings/BindServices.h"
#include "World/Script/Vm/LuauVm.h"
#include "World/Script/Vm/ScriptBindingContext.h"
#include "World/Script/Vm/ScriptRef.h"
#include "World/Script/Vm/ScriptValue.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace World
{
	namespace
	{
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
			const double volume = OptionalNumber("Audio", method, args, count, 1, 1.0);
			const double pitch = OptionalNumber("Audio", method, args, count, 2, 1.0);
			const bool loop = OptionalBool("Audio", method, args, count, 3, false);

			Audio::VoiceHandle handle = Audio::AudioSystem::Get().PostEvent2D(
				eventId,
				static_cast<float>(volume),
				static_cast<float>(pitch),
				loop);
			return ScriptValue::Number(static_cast<double>(handle));
		}

		ScriptValue AudioStopImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[2];
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
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[3];
			CheckArgumentCount("Audio", method, count, 1);
			const double handleNum = RequireNumber("Audio", method, args, count, 0);

			Audio::AudioSystem::Get().PauseVoice(static_cast<Audio::VoiceHandle>(handleNum));
			return ScriptValue::Nil();
		}

		ScriptValue AudioResumeImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[4];
			CheckArgumentCount("Audio", method, count, 1);
			const double handleNum = RequireNumber("Audio", method, args, count, 0);

			Audio::AudioSystem::Get().ResumeVoice(static_cast<Audio::VoiceHandle>(handleNum));
			return ScriptValue::Nil();
		}

		ScriptValue AudioSetLoopingImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[5];
			CheckArgumentCount("Audio", method, count, 2);
			const double handleNum = RequireNumber("Audio", method, args, count, 0);
			const bool loop = RequireBool("Audio", method, args, count, 1);

			Audio::AudioSystem::Get().SetVoiceLooping(static_cast<Audio::VoiceHandle>(handleNum), loop);
			return ScriptValue::Nil();
		}

		ScriptValue AudioStopAllImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[6];
			CheckArgumentCount("Audio", method, count, 0);
			const double fade = OptionalNumber("Audio", method, args, count, 0, 0.05);

			Audio::AudioSystem::Get().StopAll(static_cast<float>(fade));
			return ScriptValue::Nil();
		}

		ScriptValue AudioSetBusVolumeImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[7];
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
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[8];
			CheckArgumentCount("Audio", method, count, 1);
			const std::string snapshotName = RequireString("Audio", method, args, count, 0);
			const double transition = OptionalNumber("Audio", method, args, count, 1, 0.3);

			Audio::AudioSystem::Get().TriggerSnapshot(snapshotName, static_cast<float>(transition));
			return ScriptValue::Nil();
		}

		ScriptValue AudioClearSnapshotImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = AudioServiceBindings(nullptr)[0].Methods[9];
			CheckArgumentCount("Audio", method, count, 0);
			const double transition = OptionalNumber("Audio", method, args, count, 0, 0.3);

			Audio::AudioSystem::Get().ClearSnapshot(static_cast<float>(transition));
			return ScriptValue::Nil();
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
			{ "volume", "number", ScriptServiceArgType::Number, false, "Volume scale multiplier (default 1.0)." },
			{ "pitch", "number", ScriptServiceArgType::Number, false, "Pitch scale multiplier (default 1.0)." },
			{ "loop", "boolean", ScriptServiceArgType::Boolean, false, "Whether playback loops (default false)." },
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

		static const ScriptServiceMethod audioMethods[] = {
			{ "PostEvent", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioPostEventImpl(args, count);
			}, audioPostEventParams, 7, 7, "number",
				"Post a 3D sound event at world position; returns a voice handle or 0 on failure." },

			{ "PostEvent2D", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AudioPostEvent2DImpl(args, count);
			}, audioPostEvent2DParams, 4, 4, "number",
				"Post a 2D sound event without spatial attenuation; returns a voice handle or 0 on failure." },

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
}
