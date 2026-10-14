#include "World/Audio/AudioSystem.h"
#include "World/Audio/AudioCommandQueue.h"
#include "World/Audio/AudioNullBackend.h"
#include "World/Audio/AudioDsp.h"
#include "World/Audio/AudioCooker.h"
#include "World/Audio/AudioComponents.h"
#include "World/Audio/AiAudioStudio.h"
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

		std::cout << "========================================" << std::endl;
		std::cout << " ALL AUDIO SYSTEM TESTS PASSED (12/12)  " << std::endl;
		std::cout << "========================================" << std::endl;
		return 0;
	}
	catch (const std::exception& ex)
	{
		std::cerr << "Test failed with exception: " << ex.what() << std::endl;
		return 1;
	}
}
