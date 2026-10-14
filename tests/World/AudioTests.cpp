#include "World/Audio/AudioSystem.h"
#include "World/Audio/AudioCommandQueue.h"
#include "World/Audio/AudioNullBackend.h"
#include "World/Audio/AudioDsp.h"
#include "World/Audio/AudioCooker.h"
#include "World/Audio/AudioComponents.h"
#include "World/Audio/AiAudioStudio.h"
#include "World/Audio/AudioFft.h"
#include "World/Audio/SplineAudioEmitter.h"
#include "World/Audio/AudioCapture.h"
#include "World/Audio/AudioPerceptionBridge.h"
#include "World/Audio/AudioProfiler.h"
#include "World/Audio/AudioStreamer.h"
#include <fstream>
#include <filesystem>
#include "World/Core/WorldContext.h"
#include "World/Core/StringPool.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"

#include <iostream>
#include <cassert>
#include <cmath>
#include <vector>
#include <algorithm>
#include <thread>
#include <atomic>

using namespace World::Audio;

static void TestAudioBuffer()
{
	std::cout << "[Test] AudioBuffer..." << std::endl;
	AudioBuffer buf(48000, 2, 48000); // 1 second stereo
	assert(buf.GetSampleRate() == 48000);
	assert(buf.GetChannels() == 2);
	assert(buf.GetFrameCount() == 48000);
	assert(std::abs(buf.GetDurationSeconds() - 1.0f) < 0.001f);

	buf.SetSample(0, 100, 0.75f);
	buf.SetSample(1, 100, -0.5f);
	assert(std::abs(buf.GetSample(0, 100) - 0.75f) < 0.0001f);
	assert(std::abs(buf.GetSample(1, 100) - (-0.5f)) < 0.0001f);

	AudioBuffer sine = AudioBuffer::GenerateSineWave(440.0f, 0.5f, 0.8f, 48000);
	assert(sine.GetFrameCount() == 24000);
	float peak = sine.CalculatePeak();
	assert(peak > 0.75f && peak <= 0.81f);
	float rms = sine.CalculateRms();
	assert(rms > 0.5f && rms < 0.65f);

	float lufs = sine.EstimateLufs();
	sine.NormalizeLoudness(-14.0f);
	float normLufs = sine.EstimateLufs();
	assert(std::abs(normLufs - (-14.0f)) < 0.5f);
	std::cout << "  -> Passed." << std::endl;
}

static void TestAudioDsp()
{
	std::cout << "[Test] AudioDsp..." << std::endl;
	// Test Biquad LowPass
	BiquadFilter lpf;
	lpf.Configure(FilterType::LowPass, 1000.0f, 48000.0f);
	float sample = lpf.Process(1.0f);
	assert(std::abs(sample) > 0.0f);

	// Test TruePeakLimiter
	TruePeakLimiter limiter;
	limiter.Configure(-0.1f, 10.0f, 48000.0f);
	float left = 2.0f;
	float right = 2.5f;
	limiter.ProcessStereo(left, right);
	assert(left <= 1.0f && left >= -1.0f);
	assert(right <= 1.0f && right >= -1.0f);

	// Test Spatial Calculator
	float atten0 = SpatialCalculator::CalculateDistanceAttenuation(0.5f, 1.0f, 20.0f, AttenuationModel::InverseSquare);
	assert(atten0 == 1.0f);
	float attenFar = SpatialCalculator::CalculateDistanceAttenuation(25.0f, 1.0f, 20.0f, AttenuationModel::InverseSquare);
	assert(attenFar == 0.0f);

	float panL = 0.0f, panR = 0.0f;
	SpatialCalculator::CalculateStereoPan(
		glm::vec3(5.0f, 0.0f, 0.0f),  // Sound to the right
		glm::vec3(0.0f, 0.0f, 0.0f),  // Listener at origin
		glm::vec3(1.0f, 0.0f, 0.0f),  // Listener right vector
		panL, panR);
	assert(panR > panL); // Right speaker should have higher gain

	std::cout << "  -> Passed." << std::endl;
}

static void TestVoicePoolAndVirtualization()
{
	std::cout << "[Test] VoicePool & Virtualization..." << std::endl;
	VoicePool pool;
	auto testBuffer = std::make_shared<AudioBuffer>(AudioBuffer::GenerateSineWave(440.0f, 1.0f, 0.5f, 48000));

	// Allocate up to 64 physical voices
	std::vector<VoiceHandle> handles;
	for (uint32_t i = 0; i < kMaxPhysicalVoices; ++i)
	{
		VoiceHandle h = pool.AllocateVoice(testBuffer, kBusSfx, 1.0f, 1.0f, true, 128);
		assert(h != kInvalidVoiceHandle);
		handles.push_back(h);
	}
	assert(pool.GetActivePhysicalVoiceCount() == kMaxPhysicalVoices);

	// 65th voice triggers stealing
	VoiceHandle stealHandle = pool.AllocateVoice(testBuffer, kBusSfx, 1.0f, 1.0f, false, 200, StealPolicy::StealOldest);
	assert(stealHandle != kInvalidVoiceHandle);
	assert(pool.GetActivePhysicalVoiceCount() == kMaxPhysicalVoices);

	pool.StopAllVoices(0.0f);
	std::cout << "  -> Passed." << std::endl;
}

static void TestMixerGraphAndSnapshots()
{
	std::cout << "[Test] MixerGraph & Snapshots & Ducking..." << std::endl;
	MixerGraph graph;
	MixerBus* musicBus = graph.FindBus(kBusMusic);
	assert(musicBus != nullptr);
	assert(musicBus->GetName() == "Music");

	graph.SetBusVolume(kBusMusic, 0.8f);
	assert(std::abs(musicBus->GetVolume() - 0.8f) < 0.001f);

	graph.TriggerSnapshot("Snapshot_PauseMenu", 0.01f);
	// Step graph to test snapshot blend
	std::unordered_map<BusId, std::pair<std::vector<float>, std::vector<float>>> busBuffers;
	float outL[256] = { 0 };
	float outR[256] = { 0 };
	for (int i = 0; i < 10; ++i)
	{
		graph.ProcessGraph(busBuffers, outL, outR, 256, 48000.0f);
	}
	std::cout << "  -> Passed." << std::endl;
}

static void TestOfflineDeterministicNullBackend()
{
	std::cout << "[Test] Offline Deterministic NullBackend..." << std::endl;
	AudioSystem audio;
	bool ok = audio.Initialize(true, 48000, 256);
	assert(ok);

	const auto eventIds = audio.GetRegisteredSoundEventIds();
	assert(!eventIds.empty());
	assert(std::find(eventIds.begin(), eventIds.end(), "ui_click") != eventIds.end());
	assert(std::find(eventIds.begin(), eventIds.end(), "footstep_player") != eventIds.end());

	VoiceHandle vh = audio.PostEvent2D("ui_click", 1.0f, 1.0f);
	assert(vh != kInvalidVoiceHandle);

	auto* nullBackend = dynamic_cast<AudioNullBackend*>(audio.GetBackend());
	assert(nullBackend != nullptr);
	nullBackend->StepSeconds(0.2f);

	const auto& capturedL = nullBackend->GetCapturedLeft();
	assert(!capturedL.empty());
	float maxSample = 0.0f;
	for (float s : capturedL) maxSample = std::max(maxSample, std::abs(s));
	assert(maxSample > 0.05f);

	audio.Shutdown();
	std::cout << "  -> Passed." << std::endl;
}

static void TestAiAudioStudio()
{
	std::cout << "[Test] AiAudioStudio (Multi-Timbre Voice Synthesis)..." << std::endl;
	AiAudioStudioManager studio;
	auto timbres = studio.GetRegisteredTimbreIds();
	assert(!timbres.empty());

	// Test 1: Old King deep voice
	auto resKing = studio.SynthesizeLine("Old_King", "为了王国的荣耀，全军出击！", VoiceEmotion::Heroic, 0.8f);
	assert(resKing.success);
	assert(resKing.audio.GetFrameCount() > 0);
	assert(!resKing.lipSyncTrack.empty());

	// Test 2: Elf Ranger female voice
	auto resElf = studio.SynthesizeLine("Elf_Ranger", "深渊的低语正在靠近……", VoiceEmotion::Whisper, 0.6f);
	assert(resElf.success);
	assert(resElf.audio.GetFrameCount() > 0);

	// Test 3: SFX generation
	AudioBuffer laser = studio.GenerateSfxCues("sci-fi laser blast", 0.5f, 1);
	assert(laser.GetFrameCount() > 0);

	// Test 4: Music generation
	AudioBuffer music = studio.GenerateMusicTrack("battle combat", 130.0f, 2.0f);
	assert(music.GetFrameCount() > 0);

	std::cout << "  -> Passed." << std::endl;
}

static void TestCookerRoundtrip()
{
	std::cout << "[Test] AudioCooker Binary Roundtrip (PcmFloat32)..." << std::endl;
	AudioBuffer src = AudioBuffer::GenerateSineWave(440.0f, 0.2f, 0.6f, 48000);
	AudioCookSettings settings;
	settings.format = AudioFormat::PcmFloat32;
	settings.looping = true;
	settings.loopStartFrame = 100;
	settings.loopEndFrame = 5000;

	std::vector<uint8_t> binary = AudioCooker::CookToBinary(src, settings);
	assert(!binary.empty());

	AudioBuffer parsed;
	AudioCookSettings parsedSettings;
	bool parseOk = AudioCooker::ParseCookedBinary(binary.data(), binary.size(), parsed, parsedSettings);
	assert(parseOk);
	assert(parsed.GetFrameCount() == src.GetFrameCount());
	assert(parsedSettings.looping == true);
	assert(parsedSettings.loopStartFrame == 100);
	assert(parsedSettings.loopEndFrame == 5000);

	std::cout << "  -> Passed." << std::endl;
}

static void TestSoundEventYamlRoundtrip()
{
	std::cout << "[Test] SoundEvent YAML Roundtrip (Weight deserialization)..." << std::endl;
	SoundEvent evt("test_evt", kBusSfx);
	auto root = evt.GetRootContainer();
	assert(root != nullptr);
	root->variations.push_back({ "assets/audio/sfx/var1.waudio", 0.75f });
	root->variations.push_back({ "assets/audio/sfx/var2.waudio", 0.25f });

	std::string yaml = evt.SerializeYaml();
	SoundEvent loaded("temp", kBusMaster);
	bool ok = loaded.DeserializeYaml(yaml);
	assert(ok);
	assert(loaded.GetId() == "test_evt");
	assert(loaded.GetRootContainer() != nullptr);
	assert(loaded.GetRootContainer()->variations.size() == 2);
	assert(loaded.GetRootContainer()->variations[0].soundAssetPath == "assets/audio/sfx/var1.waudio");
	assert(std::abs(loaded.GetRootContainer()->variations[0].weight - 0.75f) < 0.001f);
	assert(loaded.GetRootContainer()->variations[1].soundAssetPath == "assets/audio/sfx/var2.waudio");
	assert(std::abs(loaded.GetRootContainer()->variations[1].weight - 0.25f) < 0.001f);
	std::cout << "  -> Passed." << std::endl;
}

static void TestComponentLayout()
{
	std::cout << "[Test] AudioComponent Layout Static Asserts..." << std::endl;
	static_assert(sizeof(World::AudioSourceComponent) == 40, "AudioSourceComponent must be 40B");
	static_assert(sizeof(World::AudioListenerComponent) == 8, "AudioListenerComponent must be 8B");
	static_assert(std::is_trivially_copyable_v<World::AudioSourceComponent>, "Must be trivially copyable");

	static_assert(sizeof(World::AudioListenerComponent) == 8, "AudioListenerComponent must be 8B");
	static_assert(std::is_trivially_copyable_v<World::AudioListenerComponent>, "Must be trivially copyable");

	static_assert(sizeof(World::AudioZoneComponent) == 32, "AudioZoneComponent must be 32B");
	static_assert(std::is_trivially_copyable_v<World::AudioZoneComponent>, "Must be trivially copyable");
	std::cout << "  -> Passed." << std::endl;
}

static void TestAudioCommandQueue()
{
	std::cout << "[Test] AudioCommandQueue (SPSC Lock-Free Ring Buffer)..." << std::endl;

	// 1. Basic push/pop & boundary tests on SpscQueue<int, 16>
	SpscQueue<int, 16> queue;
	assert(queue.IsEmpty());
	assert(queue.Size() == 0);
	assert(queue.GetCapacity() == 16);

	int val = 0;
	assert(!queue.Pop(val)); // Pop on empty returns false

	// Fill to capacity
	for (int i = 0; i < 16; ++i)
	{
		bool ok = queue.Push(i + 1);
		assert(ok);
	}
	assert(queue.Size() == 16);
	assert(!queue.IsEmpty());

	// Queue full: 17th push must fail
	assert(!queue.Push(999));
	assert(queue.Size() == 16);

	// Drain all items in FIFO order
	for (int i = 0; i < 16; ++i)
	{
		bool ok = queue.Pop(val);
		assert(ok);
		assert(val == i + 1);
	}
	assert(queue.IsEmpty());
	assert(queue.Size() == 0);

	// 2. Multithreaded SPSC concurrency stress test
	AudioCommandQueue cmdQueue;
	static constexpr int kTestCount = 10000;
	std::atomic<int> receivedCount{ 0 };

	std::thread producer([&cmdQueue]() {
		for (int i = 1; i <= kTestCount; ++i)
		{
			AudioCommand cmd{};
			cmd.type = AudioCommandType::SetVoiceVolume;
			cmd.voiceHandle = static_cast<VoiceHandle>(i);
			cmd.floatParam0 = static_cast<float>(i) * 0.001f;

			while (!cmdQueue.Push(cmd))
			{
				std::this_thread::yield();
			}
		}
	});

	std::thread consumer([&cmdQueue, &receivedCount]() {
		int expectedId = 1;
		while (expectedId <= kTestCount)
		{
			AudioCommand cmd{};
			if (cmdQueue.Pop(cmd))
			{
				assert(cmd.type == AudioCommandType::SetVoiceVolume);
				assert(cmd.voiceHandle == static_cast<VoiceHandle>(expectedId));
				assert(std::abs(cmd.floatParam0 - static_cast<float>(expectedId) * 0.001f) < 0.0001f);
				expectedId++;
				receivedCount.fetch_add(1, std::memory_order_relaxed);
			}
			else
			{
				std::this_thread::yield();
			}
		}
	});

	producer.join();
	consumer.join();

	assert(receivedCount.load() == kTestCount);
	assert(cmdQueue.IsEmpty());
	std::cout << "  -> Passed." << std::endl;
}

static void TestAudioAdpcmCooker()
{
	std::cout << "[Test] AudioCooker IMA-ADPCM 4:1 Codec..." << std::endl;

	// 1. Direct 16-bit PCM Encode/Decode Roundtrip
	constexpr size_t kSampleCount = 1000;
	std::vector<int16_t> origPcm(kSampleCount);
	for (size_t i = 0; i < kSampleCount; ++i)
	{
		float t = static_cast<float>(i) / 48000.0f;
		float s = std::sin(2.0f * 3.14159265f * 440.0f * t) * 0.8f;
		origPcm[i] = static_cast<int16_t>(s * 32767.0f);
	}

	std::vector<uint8_t> adpcm = AudioCooker::EncodeImaAdpcm(origPcm.data(), kSampleCount, 1);
	// 4:1 compression: 1000 16-bit samples (2000 bytes) -> 500 bytes
	assert(adpcm.size() == 500);

	std::vector<int16_t> decodedPcm = AudioCooker::DecodeImaAdpcm(adpcm.data(), kSampleCount, 1);
	assert(decodedPcm.size() == kSampleCount);

	// Check SNR / reconstruction error
	double errorEnergy = 0.0;
	double signalEnergy = 0.0;
	for (size_t i = 0; i < kSampleCount; ++i)
	{
		double diff = static_cast<double>(origPcm[i]) - static_cast<double>(decodedPcm[i]);
		errorEnergy += diff * diff;
		signalEnergy += static_cast<double>(origPcm[i]) * static_cast<double>(origPcm[i]);
	}
	double snr = 10.0 * std::log10(signalEnergy / (errorEnergy + 1e-9));
	assert(snr > 15.0); // ADPCM achieves ~20-30dB SNR

	// 2. AudioBuffer CookToBinary with AudioFormat::Adpcm
	AudioBuffer srcBuf = AudioBuffer::GenerateSineWave(440.0f, 0.1f, 0.7f, 48000);
	AudioCookSettings settings;
	settings.format = AudioFormat::Adpcm;
	settings.enableNormalization = false;
	settings.sampleRate = 48000;
	settings.channels = 2;
	settings.looping = true;
	settings.loopStartFrame = 50;
	settings.loopEndFrame = 4000;

	std::vector<uint8_t> binary = AudioCooker::CookToBinary(srcBuf, settings);
	assert(!binary.empty());

	AudioBuffer parsedBuf;
	AudioCookSettings parsedSettings;
	bool ok = AudioCooker::ParseCookedBinary(binary.data(), binary.size(), parsedBuf, parsedSettings);
	assert(ok);
	assert(parsedSettings.format == AudioFormat::Adpcm);
	assert(parsedSettings.channels == 2);
	assert(parsedSettings.sampleRate == 48000);
	assert(parsedSettings.looping == true);
	assert(parsedSettings.loopStartFrame == 50);
	assert(parsedSettings.loopEndFrame == 4000);
	assert(parsedBuf.GetFrameCount() == srcBuf.GetFrameCount());

	// Verify decoded buffer produces audio
	assert(parsedBuf.CalculatePeak() > 0.5f);
	assert(parsedBuf.CalculateRms() > 0.3f);

	std::cout << "  -> Passed." << std::endl;
}

static void TestAudioSystemEcsLifecycle()
{
	std::cout << "[Test] AudioSystem ECS Lifecycle & Spatial Sync..." << std::endl;

	AudioSystem audio;
	bool initOk = audio.Initialize(true, 48000, 256);
	assert(initOk);

	World::WorldContext context;
	World::Scene scene(context);

	// Create listener entity
	World::Entity listener = World::Entity::CreateEntity(&scene, "TestListener");
	auto& lComp = listener.AddComponent<World::AudioListenerComponent>();
	lComp.Active = true;
	auto& lTrans = listener.AddComponent<World::TransformComponent>();
	lTrans.Location = glm::vec3(0.0f, 0.0f, 0.0f);

	// Create audio source entity
	World::Entity emitter = World::Entity::CreateEntity(&scene, "TestEmitter");
	auto& sComp = emitter.AddComponent<World::AudioSourceComponent>();
	sComp.PlayOnAwake = true;
	sComp.Spatial3D = true;
	sComp.Sound.Path = World::StringPool::Get().InternPath("ui_click");
	auto& eTrans = emitter.AddComponent<World::TransformComponent>();
	eTrans.Location = glm::vec3(2.0f, 0.0f, 0.0f);

	// 1. Tick scene update: AudioSourceComponent with PlayOnAwake should automatically trigger voice
	audio.Update(0.016f, &scene);

	const auto& voiceMap = audio.GetEntityVoiceMap();
	uint32_t emitterId = static_cast<uint32_t>(emitter);
	assert(voiceMap.find(emitterId) != voiceMap.end());
	VoiceHandle handle = voiceMap.at(emitterId);
	assert(handle != kInvalidVoiceHandle);

	Voice* v = audio.GetVoicePool().FindVoice(handle);
	assert(v != nullptr);
	assert(v->IsActive());

	// 2. Move entity transform: Update should sync transform to voice
	eTrans.Location = glm::vec3(10.0f, 0.0f, 0.0f);
	audio.Update(0.016f, &scene);
	assert(voiceMap.find(emitterId) != voiceMap.end());

	// 3. Destroy entity: Update should stop voice and clear map
	scene.DestroyEntity(emitter);
	audio.Update(0.016f, &scene);
	assert(voiceMap.find(emitterId) == voiceMap.end());

	// 4. Test AudioZoneComponent AABB detection
	World::Entity zoneEnt = World::Entity::CreateEntity(&scene, "TestAudioZone");
	auto& zone = zoneEnt.AddComponent<World::AudioZoneComponent>();
	zone.Active = 1;
	zone.SnapshotId = 1;
	zone.HalfExtents[0] = 5.0f;
	zone.HalfExtents[1] = 5.0f;
	zone.HalfExtents[2] = 5.0f;
	auto& zTrans = zoneEnt.AddComponent<World::TransformComponent>();
	zTrans.Location = glm::vec3(0.0f, 0.0f, 0.0f); // Listener is at (0, 0, 0) -> inside zone

	// Register test snapshot in mixer
	MixerSnapshot snap;
	snap.name = "Snapshot_1";
	snap.busVolumes[kBusSfx] = 0.5f;
	audio.GetMixerGraph().RegisterSnapshot(snap);

	audio.Update(0.016f, &scene);

	// Move listener outside zone
	lTrans.Location = glm::vec3(20.0f, 0.0f, 0.0f);
	audio.Update(0.016f, &scene);

	audio.Shutdown();
	std::cout << "  -> Passed." << std::endl;
}

static void TestSpatial2DAndBitcrusher()
{
	std::cout << "[Test] Spatial2D & Bitcrusher..." << std::endl;

	// 1. Test Spatial2D equal-power stereo pan
	Voice voice;
	auto testBuffer = std::make_shared<AudioBuffer>(AudioBuffer::GenerateSineWave(440.0f, 0.5f, 1.0f, 48000));
	voice.Setup(1, testBuffer, kBusSfx, 1.0f, 1.0f, false);

	// Source to the right: screenPos.x = 5.0, listener.x = 0.0, halfViewWidth = 10.0, maxPan = 0.75
	voice.SetSpatial2DParameters(glm::vec2(5.0f, 0.0f), glm::vec2(0.0f, 0.0f), 10.0f, 1.0f, 25.0f, 0.75f);
	assert(voice.GetSpatialMode() == SpatialMode::Spatial2D);
	assert(voice.GetRightPanGain() > voice.GetLeftPanGain());
	float pRight = voice.GetRightPanGain();
	float pLeft = voice.GetLeftPanGain();
	float powerSum = pLeft * pLeft + pRight * pRight;
	assert(std::abs(powerSum - 1.0f) < 0.001f); // Equal power check

	// Source to the left: screenPos.x = -5.0
	voice.SetSpatial2DParameters(glm::vec2(-5.0f, 0.0f), glm::vec2(0.0f, 0.0f), 10.0f, 1.0f, 25.0f, 0.75f);
	assert(voice.GetLeftPanGain() > voice.GetRightPanGain());
	pRight = voice.GetRightPanGain();
	pLeft = voice.GetLeftPanGain();
	powerSum = pLeft * pLeft + pRight * pRight;
	assert(std::abs(powerSum - 1.0f) < 0.001f);

	// Center: screenPos.x = 0.0
	voice.SetSpatial2DParameters(glm::vec2(0.0f, 0.0f), glm::vec2(0.0f, 0.0f), 10.0f, 1.0f, 25.0f, 0.75f);
	assert(std::abs(voice.GetLeftPanGain() - voice.GetRightPanGain()) < 0.001f);

	// 2. Test XY radial distance attenuation (ignoring Z)
	// Within minDistance: dist = 0.5 < 1.0 -> full audibility (1.0)
	voice.SetSpatial2DParameters(glm::vec2(0.0f, 0.5f), glm::vec2(0.0f, 0.0f), 10.0f, 1.0f, 25.0f, 0.75f);
	assert(std::abs(voice.GetAudibility() - 1.0f) < 0.001f);

	// Beyond maxDistance: dist = 30.0 > 25.0 -> inaudible (0.0)
	voice.SetSpatial2DParameters(glm::vec2(0.0f, 30.0f), glm::vec2(0.0f, 0.0f), 10.0f, 1.0f, 25.0f, 0.75f);
	assert(voice.GetAudibility() == 0.0f);

	// 3. Test Bitcrusher DSP node
	Bitcrusher crusher;
	crusher.Configure(4, 4); // 4-bit, 4x downsample
	assert(crusher.GetBitDepth() == 4);
	assert(crusher.GetDownsampleFactor() == 4);

	float bufL[16];
	float bufR[16];
	for (int i = 0; i < 16; ++i)
	{
		bufL[i] = static_cast<float>(i) / 16.0f; // Linear ramp 0.0 to ~0.9375
		bufR[i] = -static_cast<float>(i) / 16.0f;
	}

	crusher.Process(bufL, bufR, 16);

	// Check sample-and-hold (frames 0..3 should be equal to frame 0 value)
	assert(bufL[0] == bufL[1] && bufL[1] == bufL[2] && bufL[2] == bufL[3]);
	assert(bufR[0] == bufR[1] && bufR[1] == bufR[2] && bufR[2] == bufR[3]);

	// Check quantization: 4 bits -> steps = 1 << 3 = 8, step size = 0.125
	float stepSize = 1.0f / 8.0f;
	for (int i = 0; i < 16; ++i)
	{
		float val = std::abs(bufL[i]);
		float remainder = std::fmod(val, stepSize);
		assert(remainder < 0.0001f || std::abs(remainder - stepSize) < 0.0001f);
	}

	// 4. Test Bitcrusher integrated in Voice RenderBlock
	voice.Reset();
	voice.Setup(2, testBuffer, kBusSfx, 1.0f, 1.0f, false);
	voice.SetBitcrusher(true, 4, 4);
	assert(voice.HasBitcrusher());

	float renderL[256] = { 0 };
	float renderR[256] = { 0 };
	bool stillPlaying = voice.RenderBlock(renderL, renderR, 256, 48000.0f);
	assert(stillPlaying);

	// Verify rendered audio is non-zero
	float maxVal = 0.0f;
	for (int i = 0; i < 256; ++i)
	{
		maxVal = std::max(maxVal, std::abs(renderL[i]));
	}
	assert(maxVal > 0.0f);

	std::cout << "  -> Passed." << std::endl;
}

static void TestComboEscalatorAndThrottling()
{
	std::cout << "[Test] ComboEscalator & Rapid-Fire Throttling..." << std::endl;

	AudioSystem audio;
	bool ok = audio.Initialize(true, 48000, 256);
	assert(ok);

	// 1. Test Combo Escalator pitch progression
	VoiceHandle h1 = audio.PostComboEvent("ui_click", "test_combo");
	assert(h1 != kInvalidVoiceHandle);
	const auto* esc1 = audio.GetComboEscalator("test_combo");
	assert(esc1 != nullptr);
	assert(esc1->currentCombo == 1);

	Voice* v1 = audio.GetVoicePool().FindVoice(h1);
	assert(v1 != nullptr);

	// Trigger second combo hit within timeout
	VoiceHandle h2 = audio.PostComboEvent("ui_click", "test_combo");
	assert(h2 != kInvalidVoiceHandle);
	const auto* esc2 = audio.GetComboEscalator("test_combo");
	assert(esc2->currentCombo == 2);

	Voice* v2 = audio.GetVoicePool().FindVoice(h2);
	assert(v2 != nullptr);

	// Trigger third combo hit
	VoiceHandle h3 = audio.PostComboEvent("ui_click", "test_combo");
	assert(h3 != kInvalidVoiceHandle);
	const auto* esc3 = audio.GetComboEscalator("test_combo");
	assert(esc3->currentCombo == 3);

	// Advance time past timeout (1.5s timeout)
	audio.Update(2.0f);

	// Trigger fourth combo hit after timeout: should reset to 1
	VoiceHandle h4 = audio.PostComboEvent("ui_click", "test_combo");
	assert(h4 != kInvalidVoiceHandle);
	const auto* esc4 = audio.GetComboEscalator("test_combo");
	assert(esc4->currentCombo == 1);

	// 2. Test Rapid-Fire Throttling (<10ms window, threshold = 3)
	audio.ConfigureRapidFireThrottling(3, 0.010f);

	// Rapidly trigger 5 times in the same audio frame (time does not advance)
	VoiceHandle r1 = audio.PostEvent("ui_click");
	VoiceHandle r2 = audio.PostEvent("ui_click");
	VoiceHandle r3 = audio.PostEvent("ui_click");
	assert(r1 != kInvalidVoiceHandle && r2 != kInvalidVoiceHandle && r3 != kInvalidVoiceHandle);
	// First three should be distinct allocated voices
	assert(r1 != r2 && r2 != r3 && r1 != r3);

	// 4th and 5th triggers should exceed threshold (3) and merge into 3rd voice handle
	VoiceHandle r4 = audio.PostEvent("ui_click");
	VoiceHandle r5 = audio.PostEvent("ui_click");
	assert(r4 == r3);
	assert(r5 == r3);

	// Advance time by 20ms (> 10ms window)
	audio.Update(0.020f);

	// Next trigger should be treated as new window, allocating a fresh voice
	VoiceHandle r6 = audio.PostEvent("ui_click");
	assert(r6 != kInvalidVoiceHandle);
	assert(r6 != r3);

	audio.Shutdown();
	std::cout << "  -> Passed." << std::endl;
}


static void TestHrtfAndVolumetricSpread()
{
	std::cout << "[Test] HRTF & Volumetric Spread..." << std::endl;

	// 1. Volumetric Spread
	float panL = 0.1f;
	float panR = 0.9f;
	// Far distance: distance (10.0f) >= volumetricRadius (2.0f), gains must be unchanged
	SpatialCalculator::ApplyVolumetricSpread(10.0f, 2.0f, panL, panR);
	assert(std::abs(panL - 0.1f) < 0.0001f);
	assert(std::abs(panR - 0.9f) < 0.0001f);

	// Inside radius: distance 0.0f -> full spread towards 0.7071f
	SpatialCalculator::ApplyVolumetricSpread(0.0f, 2.0f, panL, panR);
	assert(std::abs(panL - 0.7071f) < 0.001f);
	assert(std::abs(panR - 0.7071f) < 0.001f);

	// Partial spread: distance 1.0f with radius 2.0f -> 50% blend
	float initL = 0.0f, initR = 1.0f;
	SpatialCalculator::ApplyVolumetricSpread(1.0f, 2.0f, initL, initR);
	assert(std::abs(initL - 0.35355f) < 0.01f);
	assert(std::abs(initR - 0.85355f) < 0.01f);

	// 2. Binaural Cues (Front/Back pinna shadowing and elevation)
	glm::vec3 listenerPos(0.0f, 0.0f, 0.0f);
	glm::vec3 listenerRight(1.0f, 0.0f, 0.0f);
	glm::vec3 listenerForward(0.0f, 0.0f, -1.0f);
	glm::vec3 listenerUp(0.0f, 1.0f, 0.0f);

	// Sound directly in front (-Z)
	auto cuesFront = SpatialCalculator::CalculateBinauralCues(
		glm::vec3(0.0f, 0.0f, -5.0f),
		listenerPos, listenerRight, listenerForward, listenerUp);
	assert(cuesFront.rearFilterCutoff == 20000.0f);
	assert(cuesFront.rearGain == 1.0f);
	assert(std::abs(cuesFront.elevationFactor) < 0.001f);

	// Sound directly behind (+Z)
	auto cuesRear = SpatialCalculator::CalculateBinauralCues(
		glm::vec3(0.0f, 0.0f, 5.0f),
		listenerPos, listenerRight, listenerForward, listenerUp);
	assert(cuesRear.rearFilterCutoff < 10000.0f);
	assert(cuesRear.rearGain < 0.75f && cuesRear.rearGain > 0.5f); // 3~5dB attenuation

	// Sound directly above (+Y)
	auto cuesAbove = SpatialCalculator::CalculateBinauralCues(
		glm::vec3(0.0f, 5.0f, 0.0f),
		listenerPos, listenerRight, listenerForward, listenerUp);
	assert(cuesAbove.elevationFactor > 0.99f);

	// Sound directly below (-Y)
	auto cuesBelow = SpatialCalculator::CalculateBinauralCues(
		glm::vec3(0.0f, -5.0f, 0.0f),
		listenerPos, listenerRight, listenerForward, listenerUp);
	assert(cuesBelow.elevationFactor < -0.99f);

	// 3. Voice integration test
	Voice voice;
	auto testBuffer = std::make_shared<AudioBuffer>(AudioBuffer::GenerateSineWave(440.0f, 0.5f, 0.8f, 48000));
	voice.Setup(1, testBuffer, kBusSfx, 1.0f, 1.0f, false);
	voice.SetSpatialParameters(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f), 1.0f, 20.0f, AttenuationModel::InverseSquare);
	voice.SetVolumetricRadius(3.0f);
	voice.UpdateSpatial(listenerPos, listenerRight, listenerForward, glm::vec3(0.0f), 48000.0f);

	assert(voice.GetVolumetricRadius() == 3.0f);
	assert(voice.GetHrtfCutoff() < 10000.0f);

	std::cout << "  -> Passed." << std::endl;
}

static void TestAirAbsorptionAndDoppler()
{
	std::cout << "[Test] Air Absorption & Doppler..." << std::endl;

	// 1. Air Absorption Cutoff
	float cutoff0 = SpatialCalculator::CalculateAirAbsorptionCutoff(0.0f);
	assert(cutoff0 == 20000.0f);

	float cutoff50 = SpatialCalculator::CalculateAirAbsorptionCutoff(50.0f);
	assert(cutoff50 < 20000.0f && cutoff50 > 12000.0f);

	float cutoff500 = SpatialCalculator::CalculateAirAbsorptionCutoff(500.0f);
	assert(cutoff500 < cutoff50);

	float cutoffFar = SpatialCalculator::CalculateAirAbsorptionCutoff(5000.0f);
	assert(cutoffFar == 800.0f); // Clamped minimum 800Hz

	// 2. Doppler Pitch Shift
	glm::vec3 listenerPos(0.0f, 0.0f, 0.0f);
	glm::vec3 sourcePos(100.0f, 0.0f, 0.0f);

	// Stationary
	float pitchStatic = SpatialCalculator::CalculateDopplerPitch(
		sourcePos, glm::vec3(0.0f), listenerPos, glm::vec3(0.0f), 343.0f);
	assert(std::abs(pitchStatic - 1.0f) < 0.0001f);

	// Source moving towards listener (-X) at 34.3 m/s (Mach 0.1)
	// Ratio = 343 / (343 - 34.3) = 1.1111f
	float pitchApproaching = SpatialCalculator::CalculateDopplerPitch(
		sourcePos, glm::vec3(-34.3f, 0.0f, 0.0f), listenerPos, glm::vec3(0.0f), 343.0f);
	assert(std::abs(pitchApproaching - (343.0f / (343.0f - 34.3f))) < 0.01f);
	assert(pitchApproaching > 1.0f);

	// Source moving away from listener (+X) at 34.3 m/s
	// Ratio = 343 / (343 + 34.3) = 0.90909f
	float pitchReceding = SpatialCalculator::CalculateDopplerPitch(
		sourcePos, glm::vec3(34.3f, 0.0f, 0.0f), listenerPos, glm::vec3(0.0f), 343.0f);
	assert(std::abs(pitchReceding - (343.0f / (343.0f + 34.3f))) < 0.01f);
	assert(pitchReceding < 1.0f);

	// Listener moving towards source (+X) at 34.3 m/s
	// Ratio = (343 + 34.3) / 343 = 1.10f
	float pitchListenerApproaching = SpatialCalculator::CalculateDopplerPitch(
		sourcePos, glm::vec3(0.0f), listenerPos, glm::vec3(34.3f, 0.0f, 0.0f), 343.0f);
	assert(std::abs(pitchListenerApproaching - ((343.0f + 34.3f) / 343.0f)) < 0.01f);

	// Safety clamping to [0.25f, 4.0f]
	float pitchExtremeHigh = SpatialCalculator::CalculateDopplerPitch(
		sourcePos, glm::vec3(-1000.0f, 0.0f, 0.0f), listenerPos, glm::vec3(0.0f), 343.0f);
	assert(pitchExtremeHigh <= 4.0f);

	float pitchExtremeLow = SpatialCalculator::CalculateDopplerPitch(
		sourcePos, glm::vec3(10000.0f, 0.0f, 0.0f), listenerPos, glm::vec3(0.0f), 343.0f);
	assert(pitchExtremeLow >= 0.25f);

	// 3. Voice Doppler and Air Absorption Integration
	Voice voice;
	auto testBuffer = std::make_shared<AudioBuffer>(AudioBuffer::GenerateSineWave(440.0f, 0.5f, 0.8f, 48000));
	voice.Setup(2, testBuffer, kBusSfx, 1.0f, 1.0f, false);
	voice.SetSpatialParameters(sourcePos, glm::vec3(0.0f, 0.0f, -1.0f), 1.0f, 500.0f, AttenuationModel::Linear);
	voice.SetVelocity(glm::vec3(-34.3f, 0.0f, 0.0f));
	voice.UpdateSpatial(listenerPos, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f), 48000.0f);

	assert(voice.GetDopplerPitch() > 1.05f);
	assert(voice.GetAirAbsorptionCutoff() < 15000.0f);

	std::cout << "  -> Passed." << std::endl;
}

static void TestPhysicsContactSound()
{
	std::cout << "[Test] Physics Contact Sound & Zones..." << std::endl;

	AudioSystem audio;
	bool ok = audio.Initialize(true, 48000, 256);
	assert(ok);

	audio.SetPhysicsContactThreshold(1.0f);
	assert(audio.GetPhysicsContactThreshold() == 1.0f);

	// 1. Contact below threshold: no sound triggered
	audio.HandlePhysicsContact(glm::vec3(0.0f, 0.0f, 2.0f), 0.5f, "ui_click");
	assert(audio.GetVoicePool().GetActivePhysicalVoiceCount() == 0);

	// 2. Contact above threshold: sound triggered
	audio.HandlePhysicsContact(glm::vec3(0.0f, 0.0f, 2.0f), 10.0f, "ui_click");
	assert(audio.GetVoicePool().GetActivePhysicalVoiceCount() == 1);

	// 3. DSP Time Clock
	double dspTime0 = audio.GetDspTime();
	assert(dspTime0 >= 0.0);

	// Advance audio engine render frames via null backend
	audio.Update(0.1f);
	assert(audio.GetDspTime() >= dspTime0);

	// 4. AudioZone Reverb smooth transition
	assert(audio.GetCurrentZoneReverbSend() == 0.0f);
	audio.SetZoneTransitionRate(5.0f);
	assert(audio.GetZoneTransitionRate() == 5.0f);

	// 5. Listener velocity and position
	audio.SetListenerPosition(glm::vec3(10.0f, 0.0f, 0.0f));
	assert(audio.GetListenerPosition() == glm::vec3(10.0f, 0.0f, 0.0f));

	audio.Shutdown();
	std::cout << "  -> Passed." << std::endl;
}

static void TestAudioFftAndBeatDetection()
{
	std::cout << "[Test] AudioFftAndBeatDetection..." << std::endl;

	// 1. FFT Analyzer with sine wave
	AudioFftAnalyzer fft(1024);
	assert(fft.GetFftSize() == 1024);

	// Generate 1000Hz pure tone at 48kHz
	constexpr float kSampleRate = 48000.0f;
	constexpr float kFreq = 1000.0f;
	std::vector<float> sineWave(1024);
	constexpr float kPi = 3.14159265358979323846f;
	for (size_t i = 0; i < 1024; ++i)
	{
		sineWave[i] = std::sin(2.0f * kPi * kFreq * static_cast<float>(i) / kSampleRate);
	}

	fft.FeedSamplesMono(sineWave.data(), 1024);

	// Test raw magnitudes: bin corresponding to 1000Hz is 1000 * 1024 / 48000 = 21.33 -> bin 21
	std::vector<float> magnitudes;
	fft.GetRawMagnitudes(magnitudes);
	assert(magnitudes.size() == 512);

	size_t peakBin = 0;
	float peakMag = 0.0f;
	for (size_t i = 0; i < magnitudes.size(); ++i)
	{
		if (magnitudes[i] > peakMag)
		{
			peakMag = magnitudes[i];
			peakBin = i;
		}
	}
	assert(peakBin >= 19 && peakBin <= 23);
	assert(peakMag > 0.4f);

	// Test 64-band logarithmic spectrum
	std::vector<float> bands;
	fft.GetSpectrum(bands, 64);
	assert(bands.size() == 64);

	size_t peakBand = 0;
	float peakBandVal = 0.0f;
	for (size_t i = 0; i < bands.size(); ++i)
	{
		if (bands[i] > peakBandVal)
		{
			peakBandVal = bands[i];
			peakBand = i;
		}
	}
	assert(peakBand >= 33 && peakBand <= 40);

	// Test BandEnergy
	float energy1000 = fft.CalculateBandEnergy(800.0f, 1200.0f, kSampleRate);
	float energyBass = fft.CalculateBandEnergy(50.0f, 200.0f, kSampleRate);
	assert(energy1000 > energyBass * 10.0f);

	// 2. BeatDetector
	BeatDetector beat(16, 1.5f, 0.15f);
	beat.Reset();

	// Feed baseline low energy
	for (int i = 0; i < 10; ++i)
	{
		bool b = beat.Process(0.1f, 0.02f);
		assert(!b);
	}

	// Sudden kick drum impulse: energy jump to 0.8f (> 0.1 * 1.5)
	bool isBeat = beat.Process(0.8f, 0.02f);
	assert(isBeat);
	assert(beat.IsBeat());

	// Immediately subsequent frame within refractory period (e.g. 20ms < 150ms)
	bool immediateNext = beat.Process(0.85f, 0.02f);
	assert(!immediateNext);

	// Advance time past 150ms refractory period
	beat.Process(0.1f, 0.10f); // t = 120ms
	beat.Process(0.1f, 0.05f); // t = 170ms > 150ms

	// Next kick impulse triggers beat
	bool nextBeat = beat.Process(0.9f, 0.02f);
	assert(nextBeat);

	// 3. AudioSystem spectrum and beat integration
	auto& audio = AudioSystem::Get();
	audio.Initialize(true);

	std::vector<float> sysBands;
	audio.GetSpectrum(sysBands, 64);
	assert(sysBands.size() == 64);

	bool sysBeat = audio.IsBeatDetected();
	(void)sysBeat;

	audio.Shutdown();
	std::cout << "  -> Passed." << std::endl;
}

static void TestSplineAudioEmitter()
{
	std::cout << "[Test] SplineAudioEmitter..." << std::endl;

	// 3-point polyline along right angle: (0, 0, 0) -> (10, 0, 0) -> (10, 10, 0)
	std::vector<glm::vec3> points = {
		glm::vec3(0.0f, 0.0f, 0.0f),
		glm::vec3(10.0f, 0.0f, 0.0f),
		glm::vec3(10.0f, 10.0f, 0.0f)
	};

	SplineAudioEmitter spline(points, false);
	assert(spline.GetPointCount() == 3);
	assert(!spline.IsClosedLoop());

	// Listener near first segment: (3, 2, 0) -> closest should be (3, 0, 0)
	glm::vec3 p1 = spline.GetClosestPointOnSpline(glm::vec3(3.0f, 2.0f, 0.0f));
	assert(std::abs(p1.x - 3.0f) < 0.001f);
	assert(std::abs(p1.y - 0.0f) < 0.001f);
	assert(std::abs(p1.z - 0.0f) < 0.001f);
	assert(std::abs(spline.GetDistanceToSpline(glm::vec3(3.0f, 2.0f, 0.0f)) - 2.0f) < 0.001f);

	// Listener near second segment: (12, 6, 0) -> closest should be (10, 6, 0)
	glm::vec3 p2 = spline.GetClosestPointOnSpline(glm::vec3(12.0f, 6.0f, 0.0f));
	assert(std::abs(p2.x - 10.0f) < 0.001f);
	assert(std::abs(p2.y - 6.0f) < 0.001f);
	assert(std::abs(p2.z - 0.0f) < 0.001f);

	// Listener beyond endpoint: (10, 15, 0) -> closest clamped to (10, 10, 0)
	glm::vec3 p3 = spline.GetClosestPointOnSpline(glm::vec3(10.0f, 15.0f, 0.0f));
	assert(std::abs(p3.x - 10.0f) < 0.001f);
	assert(std::abs(p3.y - 10.0f) < 0.001f);

	// Volumetric spread test
	spline.SetVolumetricParameters(2.0f, 10.0f, 1.0f);
	// Distance 1.0m (< innerRadius 2.0m) -> full spread 1.0
	float sNear = spline.CalculateVolumetricSpread(glm::vec3(3.0f, 1.0f, 0.0f));
	assert(std::abs(sNear - 1.0f) < 0.001f);

	// Distance 15.0m (> outerRadius 10.0m) -> 0.0 spread (point source)
	float sFar = spline.CalculateVolumetricSpread(glm::vec3(3.0f, 25.0f, 0.0f));
	assert(std::abs(sFar - 0.0f) < 0.001f);

	// Closed loop test
	spline.SetClosedLoop(true);
	assert(spline.IsClosedLoop());
	// Segment closing (10, 10, 0) to (0, 0, 0)
	glm::vec3 pClosed = spline.GetClosestPointOnSpline(glm::vec3(5.0f, 5.0f, 0.0f));
	assert(std::abs(pClosed.x - 5.0f) < 0.01f);
	assert(std::abs(pClosed.y - 5.0f) < 0.01f);

	// AudioSystem playback lifecycle
	auto& audio = AudioSystem::Get();
	audio.Initialize(true);
	spline.Play(audio, "ui_click", 1.0f);
	assert(spline.IsPlaying());
	spline.Update(0.016f, glm::vec3(2.0f, 1.0f, 0.0f), audio);
	assert(spline.GetCurrentSpread() > 0.0f);
	spline.Stop(audio, 0.0f);
	assert(!spline.IsPlaying());
	audio.Shutdown();

	std::cout << "  -> Passed." << std::endl;
}

static void TestRadioFilterAndTypewriter()
{
	std::cout << "[Test] RadioFilterAndTypewriter..." << std::endl;

	// 1. RadioFilter test
	RadioFilter radio;
	radio.Configure(400.0f, 3500.0f, 1.8f, 48000.0f);
	assert(radio.GetHpCutoff() == 400.0f);
	assert(radio.GetLpCutoff() == 3500.0f);

	// Feed 100Hz sine (below HP 400Hz) vs 1000Hz sine (in passband)
	float maxLow = 0.0f;
	float maxMid = 0.0f;
	constexpr float kPi = 3.14159265358979323846f;
	for (int i = 0; i < 480; ++i)
	{
		float t = static_cast<float>(i) / 48000.0f;
		float sLow = std::sin(2.0f * kPi * 100.0f * t) * 0.5f;
		float outLow = radio.Process(sLow);
		maxLow = std::max(maxLow, std::abs(outLow));
	}

	radio.Reset();
	for (int i = 0; i < 480; ++i)
	{
		float t = static_cast<float>(i) / 48000.0f;
		float sMid = std::sin(2.0f * kPi * 1000.0f * t) * 0.5f;
		float outMid = radio.Process(sMid);
		maxMid = std::max(maxMid, std::abs(outMid));
	}

	// Mid band should have significantly higher transmission than sub-400Hz
	assert(maxMid > maxLow * 1.5f);

	// Stereo process check
	float left = 0.5f, right = 0.5f;
	radio.ProcessStereo(left, right);
	assert(std::abs(left) > 0.0f && std::abs(right) > 0.0f);

	// 2. TypewriterBlipThrottler test
	TypewriterBlipThrottler throttler;
	throttler.Configure(0.040f, 0.080f, 0.8f);
	throttler.Reset();

	float pitch = 1.0f;
	// Char 1 at t = 0.0s -> true
	assert(throttler.ShouldPlayCharacter('H', 0.0f, pitch));
	assert(pitch > 0.8f && pitch < 1.2f);

	// Char 2 at t = 0.020s (< 40ms) -> throttled false
	assert(!throttler.ShouldPlayCharacter('e', 0.020f, pitch));

	// Char 2 at t = 0.045s (> 40ms) -> true
	assert(throttler.ShouldPlayCharacter('e', 0.045f, pitch));

	// Space at t = 0.090s -> always false
	assert(!throttler.ShouldPlayCharacter(' ', 0.090f, pitch));

	// Punctuation '?' at t = 0.100s -> true, pitch modulated up
	assert(throttler.ShouldPlayCharacter('?', 0.100f, pitch));
	assert(pitch > 1.05f); // Curiosity inflection

	// Next char at t = 0.150s (only 50ms after punctuation; requires 40+80=120ms) -> false
	assert(!throttler.ShouldPlayCharacter('A', 0.150f, pitch));

	// Next char at t = 0.230s (130ms after punctuation > 120ms) -> true
	assert(throttler.ShouldPlayCharacter('A', 0.230f, pitch));

	std::cout << "  -> Passed." << std::endl;
}

static void TestAudioPerceptionAndCapture()
{
	std::cout << "[Test] AudioPerceptionAndCapture..." << std::endl;

	// 1. AudioPerceptionBridge test
	AudioPerceptionBridge bridge;
	int callbackCount = 0;
	SoundStimulus lastStim;

	bridge.SetStimulusCallback([&](const SoundStimulus& s) {
		callbackCount++;
		lastStim = s;
	});

	SoundStimulus stim1 { glm::vec3(10.0f, 0.0f, 5.0f), 20.0f, 0.9f, "explosion" };
	bridge.BroadcastStimulus(stim1);

	assert(callbackCount == 1);
	assert(lastStim.position == glm::vec3(10.0f, 0.0f, 5.0f));
	assert(lastStim.radius == 20.0f);
	assert(lastStim.loudness == 0.9f);
	assert(lastStim.tag == "explosion");
	assert(bridge.GetTotalStimuliBroadcast() == 1);

	auto recent = bridge.GetRecentStimuli();
	assert(recent.size() == 1);
	assert(recent[0].tag == "explosion");

	// 2. AudioSystem event perception test
	auto& audio = AudioSystem::Get();
	audio.Initialize(true);

	audio.ConfigureEventPerception("footstep_player", 15.0f, "step_noise");
	int sysCallbackCount = 0;
	SoundStimulus sysStim;
	audio.GetPerceptionBridge().SetStimulusCallback([&](const SoundStimulus& s) {
		sysCallbackCount++;
		sysStim = s;
	});

	audio.PostEvent("footstep_player", glm::vec3(2.0f, 0.0f, 3.0f), 0.5f);
	assert(sysCallbackCount == 1);
	assert(sysStim.position == glm::vec3(2.0f, 0.0f, 3.0f));
	assert(std::abs(sysStim.radius - 7.5f) < 0.01f); // 15.0f * 0.5f volumeScale
	assert(sysStim.tag == "step_noise");

	// Test AudioProfiler
	auto& profiler = audio.GetProfiler();
	auto snap = profiler.GetSnapshot();
	assert(snap.totalPhysicalVoices == kMaxPhysicalVoices);
	assert(!profiler.ToJson().empty());

	audio.Shutdown();

	// 3. AudioCaptureDevice test
	AudioCaptureDevice capture;
	auto nullBackend = std::make_unique<NullAudioCaptureBackend>();
	auto* backendRaw = nullBackend.get();
	capture.Initialize(std::move(nullBackend), 48000, 1, 4800);
	assert(!capture.IsCapturing());

	capture.Start();
	assert(capture.IsCapturing());

	// Simulate incoming microphone audio frames
	std::vector<float> micInput(256, 0.5f);
	backendRaw->SimulateMicInput(micInput.data(), 256);

	assert(capture.GetAvailableFrames() == 256);
	assert(capture.GetInputLevelRms() > 0.4f);

	std::vector<float> pulled(128);
	uint32_t readCount = capture.PopSamples(pulled.data(), 128);
	assert(readCount == 128);
	assert(std::abs(pulled[0] - 0.5f) < 0.001f);
	assert(capture.GetAvailableFrames() == 128);

	capture.ClearBuffer();
	assert(capture.GetAvailableFrames() == 0);

	capture.Stop();
	assert(!capture.IsCapturing());

	std::cout << "  -> Passed." << std::endl;
}


static void TestZeroCrossingSnap()
{
	std::cout << "[Test] Zero-Crossing Loop Snap..." << std::endl;
	AudioBuffer sine = AudioBuffer::GenerateSineWave(440.0f, 0.5f, 0.8f, 48000);

	// Pick a non-zero frame (quarter wavelength of 440Hz @ 48kHz is ~27 frames, where sine is at max peak ~0.8f)
	uint32_t zc = sine.FindZeroCrossing(27, 256);
	assert(zc != 27);
	float s0 = sine.GetSample(0, zc - 1);
	float s1 = sine.GetSample(0, zc);
	// Must transition from <= 0 to >= 0
	assert(s0 <= 0.0f && s1 >= 0.0f);
	assert(std::abs(s1) < 0.06f);

	// Test automatic loop point snapping
	sine.SetLoopPoints(30, 20000);
	sine.SnapLoopPointsToZeroCrossing(256);
	uint32_t start = sine.GetLoopStartFrame();
	uint32_t end = sine.GetLoopEndFrame();
	assert(start > 0);
	assert(end > start);
	assert(sine.GetSample(0, start - 1) <= 0.0f && sine.GetSample(0, start) >= 0.0f);
	assert(sine.GetSample(0, end - 1) <= 0.0f && sine.GetSample(0, end) >= 0.0f);

	std::cout << "  -> Passed." << std::endl;
}

static void TestAudioStreamer()
{
	std::cout << "[Test] AudioStreamer Ring Double-Buffering & Seek..." << std::endl;

	// 1. In-memory buffer streaming
	AudioBuffer testBuf = AudioBuffer::GenerateSineWave(440.0f, 0.5f, 0.7f, 48000);
	AudioStreamer streamer;
	bool ok = streamer.OpenFromBuffer(testBuf);
	assert(ok);
	assert(streamer.IsOpen());
	assert(streamer.GetTotalFrames() == testBuf.GetFrameCount());
	assert(!streamer.IsEof());

	std::vector<float> block(512 * 2);
	uint32_t read0 = streamer.ReadFrames(block.data(), 512);
	assert(read0 == 512);
	assert(std::abs(block[0] - testBuf.GetSample(0, 0)) < 0.0001f);
	assert(std::abs(block[1] - testBuf.GetSample(1, 0)) < 0.0001f);

	// Test Seek
	streamer.SeekFrame(5000);
	assert(streamer.GetCurrentFrame() == 5000);
	uint32_t readSeek = streamer.ReadFrames(block.data(), 512);
	assert(readSeek == 512);
	assert(std::abs(block[0] - testBuf.GetSample(0, 5000)) < 0.0001f);
	assert(std::abs(block[1] - testBuf.GetSample(1, 5000)) < 0.0001f);

	// 2. Real disk WAV file streaming
	std::string tempWav = "test_stream_temp.wav";
	{
		std::ofstream out(tempWav, std::ios::binary);
		uint32_t numFrames = 10000;
		uint16_t numChannels = 2;
		uint32_t sRate = 48000;
		uint16_t bitsPerSample = 16;
		uint32_t byteRate = sRate * numChannels * (bitsPerSample / 8);
		uint16_t blockAlign = numChannels * (bitsPerSample / 8);
		uint32_t dataBytes = numFrames * blockAlign;
		uint32_t riffSize = 36 + dataBytes;

		out.write("RIFF", 4);
		out.write(reinterpret_cast<const char*>(&riffSize), 4);
		out.write("WAVE", 4);
		out.write("fmt ", 4);
		uint32_t fmtSize = 16;
		uint16_t fmtTag = 1; // PCM
		out.write(reinterpret_cast<const char*>(&fmtSize), 4);
		out.write(reinterpret_cast<const char*>(&fmtTag), 2);
		out.write(reinterpret_cast<const char*>(&numChannels), 2);
		out.write(reinterpret_cast<const char*>(&sRate), 4);
		out.write(reinterpret_cast<const char*>(&byteRate), 4);
		out.write(reinterpret_cast<const char*>(&blockAlign), 2);
		out.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
		out.write("data", 4);
		out.write(reinterpret_cast<const char*>(&dataBytes), 4);

		std::vector<int16_t> pcmData(numFrames * numChannels);
		for (uint32_t i = 0; i < numFrames; ++i)
		{
			float s = std::sin(2.0f * 3.14159265f * 440.0f * (static_cast<float>(i) / 48000.0f)) * 0.5f;
			pcmData[i * 2] = static_cast<int16_t>(s * 32767.0f);
			pcmData[i * 2 + 1] = static_cast<int16_t>(s * 32767.0f);
		}
		out.write(reinterpret_cast<const char*>(pcmData.data()), dataBytes);
	}

	AudioStreamer fileStreamer;
	bool fileOk = fileStreamer.Open(tempWav);
	assert(fileOk);
	assert(fileStreamer.IsOpen());
	assert(fileStreamer.GetTotalFrames() == 10000);
	assert(fileStreamer.GetSampleRate() == 48000);
	assert(fileStreamer.GetChannels() == 2);

	std::vector<float> readBuf(2048 * 2);
	uint32_t r = fileStreamer.ReadFrames(readBuf.data(), 1024);
	assert(r == 1024);

	// Seek near end and read to EOF
	fileStreamer.SeekFrame(9000);
	assert(fileStreamer.GetCurrentFrame() == 9000);
	r = fileStreamer.ReadFrames(readBuf.data(), 1500);
	assert(r == 1000); // Only 1000 frames available until EOF
	assert(fileStreamer.IsEof());

	fileStreamer.Close();
	std::filesystem::remove(tempWav);

	std::cout << "  -> Passed." << std::endl;
}

static void TestAdvancedDspEffects()
{
	std::cout << "[Test] Advanced DSP: FDN Reverb, 5-Band EQ, Stereo Delay & Multi-Ray Occlusion..." << std::endl;

	std::cout << "    (1) FDN Reverb..." << std::endl;
	// 1. FDN 8-line Algorithmic Reverb
	{
		AlgorithmicReverb reverb;
		reverb.Configure(0.5f, 0.4f, 0.0f, 1.0f, 0.0f); // 100% wet, 0 pre-delay
		std::vector<float> left(4800, 0.0f);
		std::vector<float> right(4800, 0.0f);
		left[0] = 1.0f;
		right[0] = 1.0f; // Unit impulse

		reverb.Process(left.data(), right.data(), 4800, 48000.0f);

		float tailEnergyEarly = 0.0f;
		for (size_t i = 1200; i < 2200; ++i) tailEnergyEarly += left[i] * left[i];

		float tailEnergyLate = 0.0f;
		for (size_t i = 3500; i < 4500; ++i) tailEnergyLate += left[i] * left[i];

		assert(tailEnergyEarly > 0.0001f);
		assert(tailEnergyLate < tailEnergyEarly); // Reverb tail naturally decays
	}

	std::cout << "    (2) 5-Band EQ..." << std::endl;
	// 2. 5-Band Parametric EQ
	{
		ParametricEq eq;
		eq.SetBand(2, 12.0f, 1.0f); // +12dB boost on Band 2 (1000Hz)

		AudioBuffer sine1k = AudioBuffer::GenerateSineWave(1000.0f, 0.1f, 0.2f, 48000);
		std::vector<float> eqL(sine1k.GetFrameCount());
		std::vector<float> eqR(sine1k.GetFrameCount());
		for (size_t i = 0; i < sine1k.GetFrameCount(); ++i)
		{
			eqL[i] = sine1k.GetSample(0, static_cast<uint32_t>(i));
			eqR[i] = sine1k.GetSample(1, static_cast<uint32_t>(i));
		}

		eq.Process(eqL.data(), eqR.data(), static_cast<uint32_t>(eqL.size()), 48000.0f);

		float maxBoosted = 0.0f;
		for (float s : eqL) maxBoosted = std::max(maxBoosted, std::abs(s));
		assert(maxBoosted > 0.5f); // Peak amplified significantly by +12dB boost
	}

	std::cout << "    (3) Stereo Delay..." << std::endl;
	// 3. Stereo Delay & Ping-Pong
	{
		StereoDelay delay;
		delay.Configure(10.0f, 20.0f, 0.4f, false, 0.5f); // 10ms = 480 samples, 20ms = 960 samples @ 48kHz
		std::vector<float> delL(2000, 0.0f);
		std::vector<float> delR(2000, 0.0f);
		delL[0] = 1.0f;
		delR[0] = 1.0f;

		delay.Process(delL.data(), delR.data(), 2000, 48000.0f);
		assert(std::abs(delL[480]) > 0.1f); // Echo peak at 10ms
		assert(std::abs(delR[960]) > 0.1f); // Echo peak at 20ms
	}

	std::cout << "    (4) Multi-Ray Occlusion..." << std::endl;
	// 4. Multi-Ray Occlusion
	{
		AudioSystem& sys = AudioSystem::Get();
		sys.SetMultiRayOcclusionEnabled(true);
		sys.SetOcclusionRaycastOffsets(0.5f, 0.5f);
		sys.SetOcclusionRaycastCallback([](const glm::vec3& listener, const glm::vec3& source) -> float
		{
			// Center ray is blocked, but upward and lateral rays are clear
			if (std::abs(listener.y) < 0.1f && std::abs(listener.x) < 0.1f) return 1.0f;
			return 0.0f;
		});

		float occ = sys.CalculateMultiRayOcclusion(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 5.0f));
		// 1 blocked, 2 clear -> exactly 1/3 (0.333f)
		assert(std::abs(occ - (1.0f / 3.0f)) < 0.05f);
	}

	std::cout << "  -> Passed." << std::endl;
}


static void TestAudioRtpcSystem()
{
	std::cout << "[Test] AudioRtpc: Curve Mapping, Global & Voice Parameter Modulation..." << std::endl;

	AudioRtpcManager rtpc;
	rtpc.SetGlobalParameter("EngineRPM", 4000.0f);
	assert(rtpc.GetGlobalParameter("EngineRPM") == 4000.0f);

	// Curve: 1000 RPM -> 0.8x, 4000 RPM -> 1.4x, 8000 RPM -> 2.0x pitch
	rtpc.RegisterBinding("EngineRPM", RtpcTargetProperty::Pitch, {
		{ 1000.0f, 0.8f },
		{ 4000.0f, 1.4f },
		{ 8000.0f, 2.0f }
	});

	float vol = 1.0f, pitch = 1.0f, cutoff = 20000.0f;
	rtpc.ApplyRtpc(100, vol, pitch, cutoff);
	assert(std::abs(pitch - 1.4f) < 0.001f); // Evaluated global EngineRPM = 4000 -> 1.4x

	// Local voice parameter overrides global
	rtpc.SetVoiceParameter(100, "EngineRPM", 6000.0f);
	assert(rtpc.GetVoiceParameter(100, "EngineRPM") == 6000.0f);

	pitch = 1.0f;
	rtpc.ApplyRtpc(100, vol, pitch, cutoff);
	assert(std::abs(pitch - 1.7f) < 0.001f); // Linear interpolation between 4000 (1.4) and 8000 (2.0) is 1.7

	rtpc.RemoveVoiceParameters(100);
	pitch = 1.0f;
	rtpc.ApplyRtpc(100, vol, pitch, cutoff);
	assert(std::abs(pitch - 1.4f) < 0.001f); // Reverts to global

	std::cout << "  -> Passed." << std::endl;
}

static void TestConvolutionReverb()
{
	std::cout << "[Test] ConvolutionReverb: Impulse Response Loading & Convolution..." << std::endl;

	// Create synthetic exponential decay impulse response (0.05s = 2400 frames @ 48kHz)
	AudioBuffer irBuf(48000, 1, 2400);
	for (uint32_t i = 0; i < 2400; ++i)
	{
		float env = std::exp(-static_cast<float>(i) * 0.005f);
		float noise = (static_cast<float>(i % 17) / 17.0f) * 2.0f - 1.0f;
		irBuf.SetSample(0, i, env * noise);
	}

	ConvolutionReverb conv;
	conv.LoadImpulseResponse(irBuf);
	assert(conv.HasImpulseResponse());
	assert(conv.GetIrLengthFrames() == 2400);

	std::vector<float> inL(4800, 0.0f);
	std::vector<float> inR(4800, 0.0f);
	inL[0] = 1.0f;
	inR[0] = 1.0f; // Unit impulse

	conv.Process(inL.data(), inR.data(), 4800);

	float energy = 0.0f;
	for (size_t i = 10; i < 500; ++i) energy += inL[i] * inL[i];
	assert(energy > 0.001f);

	std::cout << "  -> Passed." << std::endl;
}

static void TestAudioFileMultiFormatLoader()
{
	std::cout << "[Test] AudioCooker: Multi-Format Loader (.wav, .ogg)..." << std::endl;

	// 1. Create temporary WAV
	std::string tempWav = "test_multiformat.wav";
	{
		std::ofstream out(tempWav, std::ios::binary);
		uint32_t numFrames = 4800;
		uint16_t numChannels = 2;
		uint32_t sRate = 48000;
		uint16_t bitsPerSample = 16;
		uint32_t byteRate = sRate * numChannels * 2;
		uint16_t blockAlign = numChannels * 2;
		uint32_t dataBytes = numFrames * blockAlign;
		uint32_t riffSize = 36 + dataBytes;

		out.write("RIFF", 4);
		out.write(reinterpret_cast<const char*>(&riffSize), 4);
		out.write("WAVE", 4);
		out.write("fmt ", 4);
		uint32_t fmtSize = 16;
		uint16_t fmtTag = 1;
		out.write(reinterpret_cast<const char*>(&fmtSize), 4);
		out.write(reinterpret_cast<const char*>(&fmtTag), 2);
		out.write(reinterpret_cast<const char*>(&numChannels), 2);
		out.write(reinterpret_cast<const char*>(&sRate), 4);
		out.write(reinterpret_cast<const char*>(&byteRate), 4);
		out.write(reinterpret_cast<const char*>(&blockAlign), 2);
		out.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
		out.write("data", 4);
		out.write(reinterpret_cast<const char*>(&dataBytes), 4);

		std::vector<int16_t> pcm(numFrames * numChannels, 1000);
		out.write(reinterpret_cast<const char*>(pcm.data()), dataBytes);
	}

	AudioBuffer wavBuf;
	bool wavOk = AudioCooker::LoadAudioFile(tempWav, wavBuf);
	assert(wavOk);
	assert(wavBuf.GetFrameCount() == 4800);
	assert(wavBuf.GetChannels() == 2);
	std::filesystem::remove(tempWav);

	// 2. Create temporary OGG with Vorbis identification header page
	std::string tempOgg = "test_multiformat.ogg";
	{
		std::ofstream out(tempOgg, std::ios::binary);
		out.write("OggS", 4);
		uint8_t ver = 0, hdrType = 2; // BOS
		out.write(reinterpret_cast<const char*>(&ver), 1);
		out.write(reinterpret_cast<const char*>(&hdrType), 1);
		uint64_t gran = 0;
		out.write(reinterpret_cast<const char*>(&gran), 8);
		uint32_t serial = 0x12345678, seq = 0, check = 0;
		out.write(reinterpret_cast<const char*>(&serial), 4);
		out.write(reinterpret_cast<const char*>(&seq), 4);
		out.write(reinterpret_cast<const char*>(&check), 4);

		uint8_t segCount = 1;
		uint8_t segVal = 30;
		out.write(reinterpret_cast<const char*>(&segCount), 1);
		out.write(reinterpret_cast<const char*>(&segVal), 1);

		// Identification packet: \x01vorbis + version(0) + channels(2) + sampleRate(44100)
		std::vector<uint8_t> id(30, 0);
		id[0] = 0x01;
		std::memcpy(id.data() + 1, "vorbis", 6);
		id[11] = 2; // channels
		*reinterpret_cast<uint32_t*>(id.data() + 12) = 44100; // sample rate
		out.write(reinterpret_cast<const char*>(id.data()), 30);
	}

	AudioBuffer oggBuf;
	bool oggOk = AudioCooker::LoadAudioFile(tempOgg, oggBuf);
	assert(oggOk);
	assert(oggBuf.GetChannels() == 2);
	assert(oggBuf.GetSampleRate() == 44100);
	std::filesystem::remove(tempOgg);

	std::cout << "  -> Passed." << std::endl;
}

static void TestProceduralSynthAndFootstep()
{
	std::cout << "[Test] ProceduralSynth & Footstep Surface System..." << std::endl;

	auto sq = ProceduralSynth::GenerateTone(ProceduralSynth::WaveType::Square, 440.0f, 0.1f, 0.5f);
	assert(sq.GetFrameCount() > 0);
	assert(sq.CalculatePeak() > 0.4f);

	auto tri = ProceduralSynth::GenerateTone(ProceduralSynth::WaveType::Triangle, 440.0f, 0.1f);
	assert(tri.GetFrameCount() > 0);

	auto saw = ProceduralSynth::GenerateTone(ProceduralSynth::WaveType::Sawtooth, 440.0f, 0.1f);
	assert(saw.GetFrameCount() > 0);

	auto noise = ProceduralSynth::GenerateTone(ProceduralSynth::WaveType::Noise, 440.0f, 0.1f);
	assert(noise.GetFrameCount() > 0);

	// Footstep sound system
	auto& audio = AudioSystem::Get();
	audio.Initialize(true);

	VoiceHandle hGrass = audio.PlayFootstep("Grass", glm::vec3(0.0f));
	assert(hGrass != kInvalidVoiceHandle);

	VoiceHandle hStone = audio.PlayFootstep("Stone", glm::vec3(1.0f));
	assert(hStone != kInvalidVoiceHandle);

	VoiceHandle hWood = audio.PlayFootstep("Wood", glm::vec3(2.0f));
	assert(hWood != kInvalidVoiceHandle);

	audio.StopAll(0.01f);
	audio.Shutdown();

	std::cout << "  -> Passed." << std::endl;
}

int main()
{
	std::cout << "========================================" << std::endl;
	std::cout << " Running WorldEngine Audio System Tests " << std::endl;
	std::cout << "========================================" << std::endl;

	try
	{
		TestAudioBuffer();
		TestAudioDsp();
		TestVoicePoolAndVirtualization();
		TestMixerGraphAndSnapshots();
		TestOfflineDeterministicNullBackend();
		TestAiAudioStudio();
		TestCookerRoundtrip();
		TestSoundEventYamlRoundtrip();
		TestComponentLayout();
		TestAudioCommandQueue();
		TestAudioAdpcmCooker();
		TestAudioSystemEcsLifecycle();
		TestSpatial2DAndBitcrusher();
		TestComboEscalatorAndThrottling();
		TestHrtfAndVolumetricSpread();
		TestAirAbsorptionAndDoppler();
		TestPhysicsContactSound();
		TestAudioFftAndBeatDetection();
		TestSplineAudioEmitter();
		TestRadioFilterAndTypewriter();
		TestAudioPerceptionAndCapture();
		TestZeroCrossingSnap();
		TestAudioStreamer();
		TestAdvancedDspEffects();
		TestAudioRtpcSystem();
		TestConvolutionReverb();
		TestAudioFileMultiFormatLoader();
		TestProceduralSynthAndFootstep();
		std::cout << "========================================" << std::endl;
		std::cout << " ALL AUDIO SYSTEM TESTS PASSED (28/28)  " << std::endl;
		std::cout << "========================================" << std::endl;
		return 0;
	}
	catch (const std::exception& ex)
	{
		std::cerr << "Test failed with exception: " << ex.what() << std::endl;
		return 1;
	}
}

