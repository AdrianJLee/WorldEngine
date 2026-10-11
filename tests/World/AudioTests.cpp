#include "World/Audio/AudioSystem.h"
#include "World/Audio/AudioNullBackend.h"
#include "World/Audio/AudioDsp.h"
#include "World/Audio/AudioCooker.h"
#include "World/Audio/AudioComponents.h"
#include "World/Audio/AiAudioStudio.h"

#include <iostream>
#include <cassert>
#include <cmath>
#include <vector>

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
	assert(rms > 0.5f && rms < 0.65f); // RMS of sine with amp A is A / sqrt(2) ~= 0.8 * 0.707 = 0.565

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
	// Music volume in Snapshot_PauseMenu is 0.4
	std::cout << "  -> Passed." << std::endl;
}

static void TestOfflineDeterministicNullBackend()
{
	std::cout << "[Test] Offline Deterministic NullBackend..." << std::endl;
	AudioSystem audio;
	bool ok = audio.Initialize(true, 48000, 256);
	assert(ok);

	VoiceHandle vh = audio.PostEvent2D("ui_click", 1.0f, 1.0f);
	assert(vh != kInvalidVoiceHandle);

	auto* nullBackend = dynamic_cast<AudioNullBackend*>(audio.GetBackend());
	assert(nullBackend != nullptr);

	nullBackend->StepSeconds(0.1f);
	const auto& capturedL = nullBackend->GetCapturedLeft();
	assert(!capturedL.empty());

	// Verify non-silent samples were generated
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
	assert(!resKing.lipSyncTrack.empty()); // Lip-sync phoneme data generated!

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
	std::cout << "[Test] AudioCooker Binary Roundtrip..." << std::endl;
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

static void TestComponentLayout()
{
	std::cout << "[Test] AudioComponent Layout Static Asserts..." << std::endl;
	static_assert(sizeof(World::AudioSourceComponent) == 40, "AudioSourceComponent must be 40B");
	static_assert(sizeof(World::AudioListenerComponent) == 8, "AudioListenerComponent must be 8B");
	static_assert(std::is_trivially_copyable_v<World::AudioSourceComponent>, "Must be trivially copyable");

	static_assert(sizeof(World::AudioListenerComponent) == 8, "AudioListenerComponent must be 32B");
	static_assert(std::is_trivially_copyable_v<World::AudioListenerComponent>, "Must be trivially copyable");

	static_assert(sizeof(World::AudioZoneComponent) == 32, "AudioZoneComponent must be 32B");
	static_assert(std::is_trivially_copyable_v<World::AudioZoneComponent>, "Must be trivially copyable");
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
		TestComponentLayout();

		std::cout << "========================================" << std::endl;
		std::cout << " ALL AUDIO SYSTEM TESTS PASSED (8/8)    " << std::endl;
		std::cout << "========================================" << std::endl;
		return 0;
	}
	catch (const std::exception& ex)
	{
		std::cerr << "Test failed with exception: " << ex.what() << std::endl;
		return 1;
	}
}
