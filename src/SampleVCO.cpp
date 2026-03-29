#include "plugin.hpp"
#include "reverbsc.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <osdialog.h>

static constexpr std::array<int, 14> kDepthMenuSteps = {
	0, 5, 10, 15, 20, 25, 30, 40, 50, 60, 70, 80, 90, 100
};

static float reverbTimeSecondsFromKnob(float knob) {
	const float t = clamp(knob, 0.f, 1.f);
	const float shaped = std::pow(t, 1.15f);
	const float minSec = 0.12f;
	const float maxSec = 10.0f;
	return minSec * std::pow(maxSec / minSec, shaped);
}

static float reverbTimeKnobFromSeconds(float seconds) {
	const float minSec = 0.12f;
	const float maxSec = 10.0f;
	const float s = clamp(seconds, minSec, maxSec);
	const float shaped = std::log(s / minSec) / std::log(maxSec / minSec);
	return std::pow(clamp(shaped, 0.f, 1.f), 1.f / 1.15f);
}

struct PitchLikeSurgeQuantity : rack::engine::ParamQuantity {
	void setDisplayValueString(std::string s) override {
		float f = std::atof(s.c_str());
		if (f > 0.f) {
			float midi = 12.f * std::log2(f / 440.f) + 69.f;
			setValue((midi - 60.f) / 12.f); // C4 at center
		}
		else {
			setValue(0.f);
		}
	}

	std::string getDisplayValueString() override {
		float note = getValue() * 12.f + 60.f; // C4 center
		float freq = 440.f * std::pow(2.f, (note - 69.f) / 12.f);
		int noteRounded = static_cast<int>(std::round(note));
		int noteClass = ((noteRounded % 12) + 12) % 12;
		int octave = static_cast<int>(std::round((noteRounded - noteClass) / 12.f - 1.f));
		static const std::array<const char*, 12> names = {
			"C", "C#", "D", "D#", "E", "F",
			"F#", "G", "G#", "A", "A#", "B"
		};
		return rack::string::f("%.2f Hz (~%s%d)", freq, names[noteClass], octave);
	}
};

struct ReverbTimeSecondsQuantity : rack::engine::ParamQuantity {
	void setDisplayValueString(std::string s) override {
		float seconds = std::atof(s.c_str());
		if (seconds > 0.f) {
			setValue(reverbTimeKnobFromSeconds(seconds));
		}
	}

	std::string getDisplayValueString() override {
		return rack::string::f("%.2f s", reverbTimeSecondsFromKnob(getValue()));
	}
};

static float sanitizeAudioOut(float v) {
	if (!std::isfinite(v)) {
		return 0.f;
	}
	return clamp(v, -10.f, 10.f);
}

static uint16_t readU16LE(const uint8_t* p) {
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static uint32_t readU32LE(const uint8_t* p) {
	return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

static bool loadWavMonoLimited5s(const std::string& path,
                                 std::vector<float>& outMono,
                                 float& outSampleRate,
                                 std::string& outError) {
	outMono.clear();
	outSampleRate = 0.f;
	outError.clear();

	std::ifstream f(path, std::ios::binary);
	if (!f.is_open()) {
		outError = "Cannot open file";
		return false;
	}

	f.seekg(0, std::ios::end);
	std::streamoff fileSize = f.tellg();
	if (fileSize < 44) {
		outError = "File too short";
		return false;
	}
	f.seekg(0, std::ios::beg);

	std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
	if (!f.read(reinterpret_cast<char*>(bytes.data()), fileSize)) {
		outError = "Read error";
		return false;
	}

	if (std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
		outError = "Not a RIFF/WAVE file";
		return false;
	}

	bool haveFmt = false;
	bool haveData = false;
	uint16_t audioFormat = 0;
	uint16_t channels = 0;
	uint16_t bitsPerSample = 0;
	uint32_t sampleRate = 0;
	const uint8_t* dataPtr = nullptr;
	size_t dataSize = 0;

	size_t offset = 12;
	while (offset + 8 <= bytes.size()) {
		const uint8_t* chunk = bytes.data() + offset;
		uint32_t chunkSize = readU32LE(chunk + 4);
		size_t chunkDataOffset = offset + 8;
		size_t chunkNext = chunkDataOffset + static_cast<size_t>(chunkSize);
		if (chunkNext > bytes.size()) {
			break;
		}

		if (std::memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16) {
			const uint8_t* fmt = bytes.data() + chunkDataOffset;
			audioFormat = readU16LE(fmt + 0);
			channels = readU16LE(fmt + 2);
			sampleRate = readU32LE(fmt + 4);
			bitsPerSample = readU16LE(fmt + 14);
			haveFmt = true;
		}
		else if (std::memcmp(chunk, "data", 4) == 0) {
			dataPtr = bytes.data() + chunkDataOffset;
			dataSize = static_cast<size_t>(chunkSize);
			haveData = true;
		}

		offset = chunkNext + (chunkSize & 1u);
	}

	if (!haveFmt || !haveData) {
		outError = "Missing fmt/data chunk";
		return false;
	}
	if (sampleRate < 1000 || channels < 1) {
		outError = "Unsupported sample rate/channels";
		return false;
	}

	const bool isPcmInt = (audioFormat == 1);
	const bool isFloat = (audioFormat == 3);
	if (!isPcmInt && !isFloat) {
		outError = "Unsupported WAV format (only PCM/float)";
		return false;
	}

	int bytesPerSample = bitsPerSample / 8;
	if (bytesPerSample < 1) {
		outError = "Unsupported bit depth";
		return false;
	}

	if (isPcmInt && !(bitsPerSample == 16 || bitsPerSample == 24 || bitsPerSample == 32)) {
		outError = "Unsupported PCM bit depth";
		return false;
	}
	if (isFloat && bitsPerSample != 32) {
		outError = "Unsupported float bit depth";
		return false;
	}

	size_t frameBytes = static_cast<size_t>(bytesPerSample) * static_cast<size_t>(channels);
	if (frameBytes == 0 || dataSize < frameBytes) {
		outError = "Invalid data chunk";
		return false;
	}

	size_t totalFrames = dataSize / frameBytes;
	size_t maxFrames = std::min(totalFrames, static_cast<size_t>(sampleRate) * 5u);
	if (maxFrames < 2) {
		outError = "Too few audio frames";
		return false;
	}

	outMono.reserve(maxFrames);
	for (size_t frame = 0; frame < maxFrames; ++frame) {
		const uint8_t* framePtr = dataPtr + frame * frameBytes;
		float sum = 0.f;
		for (uint16_t ch = 0; ch < channels; ++ch) {
			const uint8_t* s = framePtr + static_cast<size_t>(ch) * static_cast<size_t>(bytesPerSample);
			float v = 0.f;
			if (isPcmInt) {
				if (bitsPerSample == 16) {
					int16_t x = static_cast<int16_t>(s[0] | (s[1] << 8));
					v = static_cast<float>(x) / 32768.f;
				}
				else if (bitsPerSample == 24) {
					int32_t x = static_cast<int32_t>(s[0] | (s[1] << 8) | (s[2] << 16));
					if (x & 0x00800000) {
						x |= ~0x00FFFFFF;
					}
					v = static_cast<float>(x) / 8388608.f;
				}
				else {
					int32_t x = static_cast<int32_t>(readU32LE(s));
					v = static_cast<float>(x) / 2147483648.f;
				}
			}
			else {
				float x = 0.f;
				std::memcpy(&x, s, sizeof(float));
				v = x;
			}
			sum += v;
		}
		outMono.push_back(clamp(sum / static_cast<float>(channels), -1.f, 1.f));
	}

	outSampleRate = static_cast<float>(sampleRate);
	return true;
}

struct SampleVCO : Module {
	static constexpr int kMaxWavetableSize = 4096;
	static constexpr int kMaxVoices = 10; // 1 + unison(0..9)
	static constexpr int kGeneratedWavetableSize = 2048;
	static constexpr int kMipLevels = 5; // 2048,1024,512,256,128
	static constexpr float kTableTransitionTimeSec = 0.3f; // 300 ms
	static constexpr float kControlUpdateIntervalSec = 0.002f; // 2 ms control scan

	enum ParamIds {
		PITCH_PARAM,
		DETUNE_PARAM,
		UNISON_PARAM,
		OCTAVE_PARAM,
		MORPH_PARAM,
		WT_SIZE_PARAM,
		DENS_PARAM,
		SMOTH_PARAM,
		ENV_PARAM,
		RVB_TIME_PARAM,
		RVB_FB_PARAM,
		RVB_MIX_PARAM,

		MORPH_CV_DEPTH_PARAM,
		WT_SIZE_CV_DEPTH_PARAM,
		DENS_CV_DEPTH_PARAM,
		SMOTH_CV_DEPTH_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		VOCT_INPUT,
		TRIG_INPUT,
		MORPH_CV_INPUT,
		WT_SIZE_CV_INPUT,
		DENS_CV_INPUT,
		SMOTH_CV_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		LEFT_OUTPUT,
		RIGHT_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		MORPH_MOD_LIGHT,
		WT_SIZE_MOD_LIGHT,
		NUM_LIGHTS
	};

	dsp::SchmittTrigger contourTrigger;
	std::mt19937 rng {0x4e565f43u};

	std::atomic<float> tableBlend {1.f};

	struct WavetableState {
		std::array<float, kGeneratedWavetableSize> wave {};
		std::array<std::array<float, kGeneratedWavetableSize>, kMipLevels> mip {};
		std::array<int, kMipLevels> mipSize {};
		int wtSize = 1024;
		float scanNorm = 0.f;
	};

	std::array<WavetableState, 3> wavetableStates {};
	std::atomic<int> activeStateIndex {0};
	std::atomic<int> prevStateIndex {0};
	std::atomic<int> readyStateIndex {-1};
	int requestedWtSize = -1;
	int requestedDense = -1;
	int requestedSmoth = -1;
	float requestedScanNorm = -1.f;
	std::atomic<int> buildReqWtSize {1024};
	std::atomic<int> buildReqDense {100};
	std::atomic<int> buildReqSmoth {0};
	std::atomic<float> buildReqScanNorm {0.f};
	std::atomic<uint64_t> buildReqRevision {0};
	std::atomic<bool> workerRunning {false};
	std::thread workerThread;

	std::array<float, kMaxVoices> phase {};
	float controlUpdateTimer = 0.f;
	float contourEnvelope = 1.f;
	float previousSampleRate = 0.f;
	float reverbWetHpCoeff = 0.f;
	float reverbWetHpInL = 0.f;
	float reverbWetHpInR = 0.f;
	float reverbWetHpOutL = 0.f;
	float reverbWetHpOutR = 0.f;
	daisysp::ReverbSc reverb;
	std::shared_ptr<const std::vector<float>> sourceMonoActive;
	std::shared_ptr<const std::vector<float>> sourceMonoPending;
	std::shared_ptr<const std::vector<float>> sourceMonoUi;
	std::atomic<bool> sourcePendingDirty {false};
	std::atomic<float> sourceSampleRate {0.f};
	std::atomic<bool> sourceLoaded {false};
	std::string sourcePath;
	std::string sourceError;
	mutable std::mutex sourceMetaMutex;
	std::array<std::array<float, kGeneratedWavetableSize>, 2> uiDisplayWave {};
	std::atomic<int> uiDisplayWaveIndex {0};
	std::atomic<float> uiScanNorm {0.f};
	std::atomic<int> uiWtSize {1024};

	SampleVCO() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

		configInput(VOCT_INPUT, "V/Oct");
		configInput(TRIG_INPUT, "Trigger");
		configInput(MORPH_CV_INPUT, "Scan CV");
		configInput(WT_SIZE_CV_INPUT, "WT Size CV");
		configInput(DENS_CV_INPUT, "Density CV");
		configInput(SMOTH_CV_INPUT, "Smooth CV");

		configOutput(LEFT_OUTPUT, "Left");
		configOutput(RIGHT_OUTPUT, "Right");

		configParam<PitchLikeSurgeQuantity>(PITCH_PARAM, -5.f, 5.f, 0.f, "Pitch (v/oct)");
		configParam(DETUNE_PARAM, 0.f, 12.f, 0.2f, "Detune", " semitones");
		auto* unisonQ = configParam(UNISON_PARAM, 0.f, 9.f, 0.f, "Unison");
		unisonQ->snapEnabled = true;
		auto* octaveQ = configParam(OCTAVE_PARAM, -3.f, 3.f, 0.f, "Octave shift", " oct");
		octaveQ->snapEnabled = true;
		configParam(MORPH_PARAM, 0.f, 1.f, 0.5f, "Scan");
		auto* wtSizeQ = configParam(WT_SIZE_PARAM, 256.f, 2048.f, 1024.f, "WT size");
		wtSizeQ->snapEnabled = true;
		auto* densQ = configParam(DENS_PARAM, 0.f, 100.f, 100.f, "Density / simplify");
		densQ->snapEnabled = true;
		auto* smothQ = configParam(SMOTH_PARAM, 0.f, 100.f, 0.f, "Smooth");
		smothQ->snapEnabled = true;
		configParam(ENV_PARAM, 0.f, 1.f, 1.f, "Envelope");
		configParam<ReverbTimeSecondsQuantity>(RVB_TIME_PARAM, 0.f, 1.f, 0.4f, "Reverb time");
		configParam(RVB_FB_PARAM, 0.f, 1.f, 0.45f, "Reverb feedback");
		configParam(RVB_MIX_PARAM, 0.f, 1.f, 0.f, "Reverb mix");

		configParam(MORPH_CV_DEPTH_PARAM, 0.f, 1.f, 1.f, "Scan CV depth", "%", 0.f, 100.f);
		configParam(WT_SIZE_CV_DEPTH_PARAM, 0.f, 1.f, 1.f, "WT Size CV depth", "%", 0.f, 100.f);
		configParam(DENS_CV_DEPTH_PARAM, 0.f, 1.f, 1.f, "Density CV depth", "%", 0.f, 100.f);
		configParam(SMOTH_CV_DEPTH_PARAM, 0.f, 1.f, 1.f, "Smooth CV depth", "%", 0.f, 100.f);

		buildTableState(
			wavetableStates[0],
			computeWavetableSize(),
			computeDenseParam(),
			computeSmothParam(),
			clamp(computeScanParam(), 0.f, 1.f),
			std::shared_ptr<const std::vector<float>> {}
		);
		wavetableStates[1] = wavetableStates[0];
		wavetableStates[2] = wavetableStates[0];
		activeStateIndex.store(0, std::memory_order_relaxed);
		prevStateIndex.store(0, std::memory_order_relaxed);
		readyStateIndex.store(-1, std::memory_order_relaxed);
		publishUiDisplayWave();
		submitBuildRequest(computeWavetableSize(), computeDenseParam(), computeSmothParam(), clamp(computeScanParam(), 0.f, 1.f));
		startWorkerThread();
		reverb.Init(48000.f);
		updateReverbWetHighpass(48000.f);
		resetReverbWetHighpass();
	}

	~SampleVCO() override {
		stopWorkerThread();
	}

	float readWavetableLevelSample(const std::array<std::array<float, kGeneratedWavetableSize>, kMipLevels>& mip,
	                               const std::array<int, kMipLevels>& mipSizes,
	                               int level,
	                               float ph) const {
		auto wrapIndex = [](int i, int size) {
			int r = i % size;
			return (r < 0) ? (r + size) : r;
		};
		auto hermite4 = [](float xm1, float x0, float x1, float x2, float t) {
			float c0 = x0;
			float c1 = 0.5f * (x1 - xm1);
			float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
			float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
			return ((c3 * t + c2) * t + c1) * t + c0;
		};

		int l = clamp(level, 0, kMipLevels - 1);
		int sizeLocal = mipSizes[l];
		if (sizeLocal < 2) {
			return 0.f;
		}

		float pos = ph * static_cast<float>(sizeLocal - 1);
		int i0 = static_cast<int>(std::floor(pos));
		int i1 = wrapIndex(i0 + 1, sizeLocal);
		float frac = pos - static_cast<float>(i0);

		i0 = wrapIndex(i0, sizeLocal);
		float s0 = mip[l][i0];
		float s1 = mip[l][i1];
		float linear = s0 + (s1 - s0) * frac;

		if (sizeLocal < 4) {
			return sanitizeWaveSample(linear);
		}

		int im1 = wrapIndex(i0 - 1, sizeLocal);
		int i2 = wrapIndex(i0 + 2, sizeLocal);
		float cubic = hermite4(mip[l][im1], s0, s1, mip[l][i2], frac);
		return sanitizeWaveSample(cubic);
	}

	void selectMipLevels(float freq, int& level0, int& level1, float& blend) const {
		float sr = std::max(previousSampleRate, 1000.f);
		float f = std::max(std::abs(freq), 1.f);
		// Required table size for current pitch to keep top harmonic near Nyquist.
		float desiredSize = clamp(sr / f, 128.f, static_cast<float>(kGeneratedWavetableSize));
		float rawLevel = std::log2(static_cast<float>(kGeneratedWavetableSize) / desiredSize);
		// Slight upward bias reduces alias-prone highs at the expense of tiny HF roll-off.
		float levelF = clamp(rawLevel + 0.20f, 0.f, static_cast<float>(kMipLevels - 1));
		level0 = static_cast<int>(std::floor(levelF));
		level1 = std::min(level0 + 1, kMipLevels - 1);
		blend = levelF - static_cast<float>(level0);
	}

	float readMorphSample(float ph, float freq) {
		int level0 = 0;
		int level1 = 0;
		float levelBlend = 0.f;
		selectMipLevels(freq, level0, level1, levelBlend);

		int activeIdx = activeStateIndex.load(std::memory_order_acquire);
		int prevIdx = prevStateIndex.load(std::memory_order_acquire);
		const WavetableState& active = wavetableStates[activeIdx];
		const WavetableState& prevState = wavetableStates[prevIdx];

		float curr0 = readWavetableLevelSample(active.mip, active.mipSize, level0, ph);
		float curr1 = readWavetableLevelSample(active.mip, active.mipSize, level1, ph);
		float curr = sanitizeWaveSample(curr0 + (curr1 - curr0) * levelBlend);

		float prev0 = readWavetableLevelSample(prevState.mip, prevState.mipSize, level0, ph);
		float prev1 = readWavetableLevelSample(prevState.mip, prevState.mipSize, level1, ph);
		float prev = sanitizeWaveSample(prev0 + (prev1 - prev0) * levelBlend);

		float blend = clamp(tableBlend.load(std::memory_order_relaxed), 0.f, 1.f);
		return sanitizeWaveSample(prev + (curr - prev) * blend);
	}

	float getModulatedKnobValue(float baseValue, int cvInputId, int depthParamId, float minV, float maxV) {
		float depth = clamp(params[depthParamId].getValue(), 0.f, 1.f);
		float v = baseValue;
		if (inputs[cvInputId].isConnected()) {
			float cvNorm = clamp(inputs[cvInputId].getVoltage() / 5.f, -1.f, 1.f);
			float halfRange = 0.5f * (maxV - minV) * depth;
			v += cvNorm * halfRange;
		}
		return clamp(v, minV, maxV);
	}

	float computeScanParam() {
		return getModulatedKnobValue(params[MORPH_PARAM].getValue(), MORPH_CV_INPUT, MORPH_CV_DEPTH_PARAM, 0.f, 1.f);
	}

	bool loadSourceWavPath(const std::string& path) {
		std::vector<float> mono;
		float sr = 0.f;
		std::string err;
		if (!loadWavMonoLimited5s(path, mono, sr, err)) {
			std::shared_ptr<const std::vector<float>> empty;
			std::atomic_store_explicit(&sourceMonoPending, empty, std::memory_order_release);
			std::atomic_store_explicit(&sourceMonoUi, empty, std::memory_order_release);
			sourceLoaded.store(false, std::memory_order_relaxed);
			sourceSampleRate.store(0.f, std::memory_order_relaxed);
			{
				std::lock_guard<std::mutex> lock(sourceMetaMutex);
				sourcePath.clear();
				sourceError = err;
			}
			sourcePendingDirty.store(true, std::memory_order_relaxed);
			return false;
		}

		auto monoPtr = std::make_shared<const std::vector<float>>(std::move(mono));
		std::atomic_store_explicit(&sourceMonoPending, monoPtr, std::memory_order_release);
		std::atomic_store_explicit(&sourceMonoUi, monoPtr, std::memory_order_release);
		sourceLoaded.store(true, std::memory_order_relaxed);
		sourceSampleRate.store(sr, std::memory_order_relaxed);
		{
			std::lock_guard<std::mutex> lock(sourceMetaMutex);
			sourcePath = path;
			sourceError.clear();
		}
		sourcePendingDirty.store(true, std::memory_order_relaxed);
		return true;
	}

	void clearSourceWav() {
		std::shared_ptr<const std::vector<float>> empty;
		std::atomic_store_explicit(&sourceMonoPending, empty, std::memory_order_release);
		std::atomic_store_explicit(&sourceMonoUi, empty, std::memory_order_release);
		sourceLoaded.store(false, std::memory_order_relaxed);
		sourceSampleRate.store(0.f, std::memory_order_relaxed);
		{
			std::lock_guard<std::mutex> lock(sourceMetaMutex);
			sourcePath.clear();
			sourceError.clear();
		}
		sourcePendingDirty.store(true, std::memory_order_relaxed);
	}

	std::string getSourceStatusString() const {
		auto sourcePtr = std::atomic_load_explicit(&sourceMonoUi, std::memory_order_acquire);
		float sr = sourceSampleRate.load(std::memory_order_relaxed);
		if (sourcePtr && !sourcePtr->empty() && sr > 1000.f && sourceLoaded.load(std::memory_order_relaxed)) {
			float sec = static_cast<float>(sourcePtr->size()) / sr;
			return rack::string::f("WAV %.2fs", sec);
		}
		{
			std::lock_guard<std::mutex> lock(sourceMetaMutex);
			if (!sourceError.empty()) {
				return std::string("WAV ERR: ") + sourceError;
			}
		}
		return "RANDOM SOURCE";
	}

	void copySourceOverviewData(std::array<float, kMaxWavetableSize>& outData,
	                            int& outSize,
	                            float& outWindowStartNorm,
	                            float& outWindowSpanNorm) const {
		auto sourcePtr = std::atomic_load_explicit(&sourceMonoUi, std::memory_order_acquire);
		outWindowStartNorm = 0.f;
		outWindowSpanNorm = 0.f;
		if (!sourcePtr || sourcePtr->size() < 2 || !sourceLoaded.load(std::memory_order_relaxed)) {
			outSize = 0;
			return;
		}

		const int displaySamples = 1024;
		outSize = displaySamples;
		const int srcSize = static_cast<int>(sourcePtr->size());
		int windowFrames = clamp(uiWtSize.load(std::memory_order_relaxed), 256, kGeneratedWavetableSize);
		windowFrames = std::min(windowFrames, srcSize);
		int maxStart = std::max(0, srcSize - windowFrames);
		float scan = clamp(uiScanNorm.load(std::memory_order_relaxed), 0.f, 1.f);
		int start = static_cast<int>(std::lround(scan * static_cast<float>(maxStart)));
		start = clamp(start, 0, maxStart);
		outWindowStartNorm = (srcSize > 1) ? (static_cast<float>(start) / static_cast<float>(srcSize - 1)) : 0.f;
		outWindowSpanNorm = clamp(static_cast<float>(windowFrames) / static_cast<float>(srcSize), 0.f, 1.f);

		for (int i = 0; i < displaySamples; ++i) {
			float t = static_cast<float>(i) / static_cast<float>(displaySamples - 1);
			float pos = t * static_cast<float>(srcSize - 1);
			int i0 = static_cast<int>(std::floor(pos));
			int i1 = std::min(i0 + 1, srcSize - 1);
			float frac = pos - static_cast<float>(i0);
			float s0 = (*sourcePtr)[i0];
			float s1 = (*sourcePtr)[i1];
			outData[i] = sanitizeWaveSample(s0 + (s1 - s0) * frac);
		}
	}

	void updateReverbWetHighpass(float sampleRate) {
		constexpr float cutoffHz = 110.f;
		float sr = std::max(sampleRate, 1000.f);
		reverbWetHpCoeff = std::exp(-2.f * 3.14159265359f * cutoffHz / sr);
	}

	void resetReverbWetHighpass() {
		reverbWetHpInL = 0.f;
		reverbWetHpInR = 0.f;
		reverbWetHpOutL = 0.f;
		reverbWetHpOutR = 0.f;
	}

	void processReverbWetHighpass(float& wetL, float& wetR) {
		float yL = wetL - reverbWetHpInL + reverbWetHpCoeff * reverbWetHpOutL;
		float yR = wetR - reverbWetHpInR + reverbWetHpCoeff * reverbWetHpOutR;

		reverbWetHpInL = wetL;
		reverbWetHpInR = wetR;
		reverbWetHpOutL = std::isfinite(yL) ? yL : 0.f;
		reverbWetHpOutR = std::isfinite(yR) ? yR : 0.f;
		wetL = reverbWetHpOutL;
		wetR = reverbWetHpOutR;
	}

	float sanitizeWaveSample(float v) const {
		if (!std::isfinite(v)) {
			return 0.f;
		}
		return clamp(v, -1.f, 1.f);
	}

	int computeWavetableSize() {
		float v = getModulatedKnobValue(params[WT_SIZE_PARAM].getValue(), WT_SIZE_CV_INPUT,
		                                WT_SIZE_CV_DEPTH_PARAM, 256.f, 2048.f);
		return clamp(static_cast<int>(std::round(v)), 256, 2048);
	}

	int computeDenseParam() {
		float v = getModulatedKnobValue(params[DENS_PARAM].getValue(), DENS_CV_INPUT,
		                                DENS_CV_DEPTH_PARAM, 0.f, 100.f);
		return clamp(static_cast<int>(std::round(v)), 0, 100);
	}

	int computeSmothParam() {
		float v = getModulatedKnobValue(params[SMOTH_PARAM].getValue(), SMOTH_CV_INPUT,
		                                SMOTH_CV_DEPTH_PARAM, 0.f, 100.f);
		return clamp(static_cast<int>(std::round(v)), 0, 100);
	}

	float computeEnvParam() {
		return clamp(params[ENV_PARAM].getValue(), 0.f, 1.f);
	}

	float processEnvEnvelope(float trigVoltage, bool trigPatched, float env, float sampleTime) {
		if (!trigPatched || env >= 0.999f) {
			contourEnvelope = 1.f;
			return contourEnvelope;
		}

		auto expInterp = [](float minV, float maxV, float t) {
			if (minV <= 0.f || maxV <= minV) {
				return minV;
			}
			return minV * std::pow(maxV / minV, clamp(t, 0.f, 1.f));
		};

		bool gateHigh = trigVoltage >= 1.f;
		if (contourTrigger.process(trigVoltage)) {
			// De-click: avoid hard jump to 1.0 on trigger edge.
			// Let attack stage ramp naturally.
			if (contourEnvelope < 0.25f) {
				contourEnvelope = 0.f;
			}
		}

		float e = clamp(env, 0.f, 1.f);
		float attackShape = std::pow(e, 2.2f);
		float releaseShape = std::pow(e, 1.35f);
		const float attackSec = expInterp(0.0018f, 0.020f, attackShape);
		const float releaseSec = expInterp(0.006f, 5.5f, releaseShape);
		float attackStep = clamp(sampleTime / std::max(attackSec, 1e-4f), 0.f, 1.f);
		float releaseStep = clamp(sampleTime / std::max(releaseSec, 1e-4f), 0.f, 1.f);

		if (gateHigh) {
			contourEnvelope += (1.f - contourEnvelope) * attackStep;
		}
		else {
			contourEnvelope += (0.f - contourEnvelope) * releaseStep;
		}

		contourEnvelope = clamp(contourEnvelope, 0.f, 1.f);
		return contourEnvelope;
	}

	void rebuildMipmapsFromTable(
		const std::array<float, kGeneratedWavetableSize>& source,
		std::array<std::array<float, kGeneratedWavetableSize>, kMipLevels>& mipOut,
		std::array<int, kMipLevels>& mipSizesOut) {
		const int baseSize = kGeneratedWavetableSize;
		mipSizesOut[0] = baseSize;
		for (int i = 0; i < baseSize; ++i) {
			mipOut[0][i] = sanitizeWaveSample(source[i]);
		}
		mipOut[0][0] = 0.f;
		mipOut[0][baseSize - 1] = 0.f;

		for (int level = 1; level < kMipLevels; ++level) {
			int prevSize = mipSizesOut[level - 1];
			int size = std::max(128, prevSize / 2);
			mipSizesOut[level] = size;

			for (int i = 0; i < size; ++i) {
				int i0 = std::min(i * 2, prevSize - 1);
				int i1 = std::min(i0 + 1, prevSize - 1);
				float a = mipOut[level - 1][i0];
				float b = mipOut[level - 1][i1];
				mipOut[level][i] = sanitizeWaveSample(0.5f * (a + b));
			}

			for (int i = size; i < kGeneratedWavetableSize; ++i) {
				mipOut[level][i] = 0.f;
			}
			mipOut[level][0] = 0.f;
			mipOut[level][size - 1] = 0.f;
		}
	}

	void buildTableState(WavetableState& outState,
	                     int wtSizeParam,
	                     int dens,
	                     int smoth,
	                     float scanNorm,
	                     const std::shared_ptr<const std::vector<float>>& sourcePtr) {
		float smothF = clamp(smoth * 0.01f, 0.f, 1.f);
		float densF = clamp(dens * 0.01f, 0.f, 1.f);
		int windowFrames = clamp(wtSizeParam, 256, kGeneratedWavetableSize);
		bool sourceReady = (sourcePtr && sourcePtr->size() > 2);
		std::array<float, kGeneratedWavetableSize> localWindow {};

		if (sourceReady) {
			int srcSize = static_cast<int>(sourcePtr->size());
			windowFrames = std::min(windowFrames, srcSize);
			int maxStart = std::max(0, srcSize - windowFrames);
			int start = static_cast<int>(std::lround(clamp(scanNorm, 0.f, 1.f) * static_cast<float>(maxStart)));
			start = clamp(start, 0, maxStart);
			for (int i = 0; i < windowFrames; ++i) {
				localWindow[i] = sanitizeWaveSample((*sourcePtr)[start + i]);
			}
		}
		else {
			std::uniform_real_distribution<float> dist(-1.f, 1.f);
			for (int i = 0; i < windowFrames; ++i) {
				localWindow[i] = dist(rng);
			}
		}

		int truePoints = static_cast<int>(std::lround(64.f + densF * static_cast<float>(windowFrames - 64)));
		truePoints = clamp(truePoints, 64, windowFrames);

		std::array<int, kGeneratedWavetableSize> anchorIdx {};
		int prev = 0;
		for (int p = 0; p < truePoints; ++p) {
			float t = (truePoints <= 1) ? 0.f : static_cast<float>(p) / static_cast<float>(truePoints - 1);
			int idx = static_cast<int>(std::lround(t * static_cast<float>(windowFrames - 1)));
			int remaining = truePoints - 1 - p;
			int maxAllowed = (windowFrames - 1) - remaining;
			idx = clamp(idx, prev, maxAllowed);
			anchorIdx[p] = idx;
			prev = idx + 1;
		}

		std::array<float, kGeneratedWavetableSize> simplified = localWindow;
		const float pi = 3.14159265359f;
		for (int p = 0; p < truePoints - 1; ++p) {
			int i0 = anchorIdx[p];
			int i1 = anchorIdx[p + 1];
			float y0 = localWindow[i0];
			float y1 = localWindow[i1];
			int span = std::max(1, i1 - i0);
			for (int i = i0; i <= i1; ++i) {
				float t = static_cast<float>(i - i0) / static_cast<float>(span);
				float linear = y0 + (y1 - y0) * t;
				float sinusT = 0.5f - 0.5f * std::cos(pi * t);
				float sinus = y0 + (y1 - y0) * sinusT;
				simplified[i] = sanitizeWaveSample(linear + (sinus - linear) * smothF);
			}
		}
		for (int p = 0; p < truePoints; ++p) {
			int idx = anchorIdx[p];
			simplified[idx] = localWindow[idx];
		}
		localWindow = simplified;

		const float outDen = static_cast<float>(kGeneratedWavetableSize - 1);
		const float srcDen = static_cast<float>(windowFrames - 1);
		for (int i = 0; i < kGeneratedWavetableSize; ++i) {
			float pos = (static_cast<float>(i) / outDen) * srcDen;
			int i0 = static_cast<int>(std::floor(pos));
			int i1 = std::min(i0 + 1, windowFrames - 1);
			float frac = pos - static_cast<float>(i0);
			float s0 = localWindow[i0];
			float s1 = localWindow[i1];
			outState.wave[i] = sanitizeWaveSample(s0 + (s1 - s0) * frac);
		}

		// Keep seamless table boundaries stable.
		const int edgeSamples = 64;
		const int edge = std::min(edgeSamples, kGeneratedWavetableSize / 2);
		for (int i = 0; i < edge; ++i) {
			float t = static_cast<float>(i) / static_cast<float>(std::max(1, edge - 1));
			float w = 0.5f - 0.5f * std::cos(pi * t);
			outState.wave[i] = sanitizeWaveSample(outState.wave[i] * w);
			outState.wave[kGeneratedWavetableSize - 1 - i] = sanitizeWaveSample(outState.wave[kGeneratedWavetableSize - 1 - i] * w);
		}
		outState.wave[0] = 0.f;
		outState.wave[kGeneratedWavetableSize - 1] = 0.f;
		outState.wtSize = windowFrames;
		outState.scanNorm = clamp(scanNorm, 0.f, 1.f);
		rebuildMipmapsFromTable(outState.wave, outState.mip, outState.mipSize);
	}

	int acquireBuildSlot() const {
		int active = activeStateIndex.load(std::memory_order_acquire);
		int prev = prevStateIndex.load(std::memory_order_acquire);
		int ready = readyStateIndex.load(std::memory_order_acquire);
		for (int i = 0; i < static_cast<int>(wavetableStates.size()); ++i) {
			if (i != active && i != prev && i != ready) {
				return i;
			}
		}
		return -1;
	}

	void submitBuildRequest(int wtSize, int dens, int smoth, float scanNorm) {
		buildReqWtSize.store(clamp(wtSize, 256, kGeneratedWavetableSize), std::memory_order_relaxed);
		buildReqDense.store(clamp(dens, 0, 100), std::memory_order_relaxed);
		buildReqSmoth.store(clamp(smoth, 0, 100), std::memory_order_relaxed);
		buildReqScanNorm.store(clamp(scanNorm, 0.f, 1.f), std::memory_order_relaxed);
		buildReqRevision.fetch_add(1, std::memory_order_release);
	}

	void startWorkerThread() {
		workerRunning.store(true, std::memory_order_release);
		workerThread = std::thread([this]() {
			uint64_t seenRevision = buildReqRevision.load(std::memory_order_acquire);
			while (workerRunning.load(std::memory_order_acquire)) {
				uint64_t revision = buildReqRevision.load(std::memory_order_acquire);
				if (revision == seenRevision) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}

				int slot = acquireBuildSlot();
				if (slot < 0) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}

				int wtSize = buildReqWtSize.load(std::memory_order_relaxed);
				int dens = buildReqDense.load(std::memory_order_relaxed);
				int smoth = buildReqSmoth.load(std::memory_order_relaxed);
				float scanNorm = buildReqScanNorm.load(std::memory_order_relaxed);
				auto sourcePtr = std::atomic_load_explicit(&sourceMonoActive, std::memory_order_acquire);
				buildTableState(wavetableStates[slot], wtSize, dens, smoth, scanNorm, sourcePtr);

				uint64_t revisionAfterBuild = buildReqRevision.load(std::memory_order_acquire);
				if (revisionAfterBuild != revision) {
					seenRevision = revision;
					continue;
				}

				readyStateIndex.store(slot, std::memory_order_release);
				seenRevision = revision;
			}
		});
	}

	void stopWorkerThread() {
		workerRunning.store(false, std::memory_order_release);
		if (workerThread.joinable()) {
			workerThread.join();
		}
	}

	void publishUiDisplayWave() {
		int next = 1 - uiDisplayWaveIndex.load(std::memory_order_relaxed);
		float blend = clamp(tableBlend.load(std::memory_order_relaxed), 0.f, 1.f);
		int activeIdx = activeStateIndex.load(std::memory_order_acquire);
		int prevIdx = prevStateIndex.load(std::memory_order_acquire);
		const WavetableState& active = wavetableStates[activeIdx];
		const WavetableState& prev = wavetableStates[prevIdx];
		for (int i = 0; i < kGeneratedWavetableSize; ++i) {
			float prevSample = prev.wave[i];
			float currSample = active.wave[i];
			uiDisplayWave[next][i] = sanitizeWaveSample(prevSample + (currSample - prevSample) * blend);
		}
		uiDisplayWaveIndex.store(next, std::memory_order_release);
		uiScanNorm.store(active.scanNorm, std::memory_order_relaxed);
		uiWtSize.store(active.wtSize, std::memory_order_relaxed);
	}

	void copyDisplayData(std::array<float, kMaxWavetableSize>& outData, int& outSize, float& outScan) const {
		int idx = uiDisplayWaveIndex.load(std::memory_order_acquire);
		outSize = kGeneratedWavetableSize;
		for (int i = 0; i < outSize; ++i) {
			outData[i] = uiDisplayWave[idx][i];
		}
		outScan = uiScanNorm.load(std::memory_order_relaxed);
	}

	int getPublishedWtSize() const {
		return uiWtSize.load(std::memory_order_relaxed);
	}

	void onReset() override {
		for (float& p : phase) {
			p = 0.f;
		}
		int active = activeStateIndex.load(std::memory_order_acquire);
		prevStateIndex.store(active, std::memory_order_release);
		readyStateIndex.store(-1, std::memory_order_release);
		tableBlend.store(1.f, std::memory_order_relaxed);
		requestedWtSize = -1;
		requestedDense = -1;
		requestedSmoth = -1;
		requestedScanNorm = -1.f;
		submitBuildRequest(computeWavetableSize(), computeDenseParam(), computeSmothParam(), clamp(computeScanParam(), 0.f, 1.f));
		publishUiDisplayWave();
		controlUpdateTimer = 0.f;
		contourEnvelope = 1.f;
		float sr = previousSampleRate > 1.f ? previousSampleRate : 48000.f;
		reverb.Init(sr);
		updateReverbWetHighpass(sr);
		resetReverbWetHighpass();
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		{
			std::lock_guard<std::mutex> lock(sourceMetaMutex);
			if (!sourcePath.empty()) {
				json_object_set_new(rootJ, "sourcePath", json_string(sourcePath.c_str()));
			}
		}
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* sourcePathJ = json_object_get(rootJ, "sourcePath");
		if (json_is_string(sourcePathJ)) {
			loadSourceWavPath(json_string_value(sourcePathJ));
		}
	}

	void updateTablesIfNeeded() {
		bool sourceChanged = false;
		if (sourcePendingDirty.exchange(false, std::memory_order_relaxed)) {
			auto pending = std::atomic_load_explicit(&sourceMonoPending, std::memory_order_acquire);
			std::atomic_store_explicit(&sourceMonoActive, pending, std::memory_order_release);
			sourceChanged = true;
		}

		int targetSize = computeWavetableSize();
		int targetDense = computeDenseParam();
		int targetSmoth = computeSmothParam();
		float targetScan = clamp(computeScanParam(), 0.f, 1.f);

		bool targetChanged = sourceChanged ||
		                     (targetSize != requestedWtSize) ||
		                     (targetDense != requestedDense) ||
		                     (targetSmoth != requestedSmoth) ||
		                     (std::abs(targetScan - requestedScanNorm) > 1e-4f);
		if (targetChanged) {
			requestedWtSize = targetSize;
			requestedDense = targetDense;
			requestedSmoth = targetSmoth;
			requestedScanNorm = targetScan;
			submitBuildRequest(targetSize, targetDense, targetSmoth, targetScan);
		}

		int ready = readyStateIndex.exchange(-1, std::memory_order_acq_rel);
		if (ready >= 0) {
			int active = activeStateIndex.load(std::memory_order_acquire);
			if (ready != active) {
				prevStateIndex.store(active, std::memory_order_release);
				activeStateIndex.store(ready, std::memory_order_release);
				tableBlend.store(0.f, std::memory_order_relaxed);
			}
			publishUiDisplayWave();
		}

		float blend = tableBlend.load(std::memory_order_relaxed);
		if (blend >= 0.999f) {
			int active = activeStateIndex.load(std::memory_order_acquire);
			prevStateIndex.store(active, std::memory_order_release);
		}
		if (blend < 0.999f) {
			publishUiDisplayWave();
		}
	}

	void process(const ProcessArgs& args) override {
		if (std::abs(args.sampleRate - previousSampleRate) > 1.f) {
			previousSampleRate = args.sampleRate;
			reverb.Init(args.sampleRate);
			updateReverbWetHighpass(args.sampleRate);
			resetReverbWetHighpass();
		}

		controlUpdateTimer += args.sampleTime;
		while (controlUpdateTimer >= kControlUpdateIntervalSec) {
			controlUpdateTimer -= kControlUpdateIntervalSec;
			updateTablesIfNeeded();
		}

		float voct = inputs[VOCT_INPUT].getVoltage();
		float pitchOct = params[PITCH_PARAM].getValue() + params[OCTAVE_PARAM].getValue() + voct;
		int unison = clamp(static_cast<int>(std::round(params[UNISON_PARAM].getValue())), 0, 9);
		int voices = 1 + unison;
		float detuneSemitones = params[DETUNE_PARAM].getValue();

		float outL = 0.f;
		float outR = 0.f;
		for (int v = 0; v < voices; ++v) {
			float spread = 0.f;
			if (voices > 1) {
				spread = (static_cast<float>(v) / static_cast<float>(voices - 1)) * 2.f - 1.f;
			}
			float detuneRatio = std::pow(2.f, (spread * detuneSemitones) / 12.f);
			float freq = dsp::FREQ_C4 * std::pow(2.f, pitchOct) * detuneRatio;
			freq = clamp(freq, 0.f, 20000.f);
			phase[v] += freq * args.sampleTime;
			if (phase[v] >= 1.f) {
				phase[v] -= std::floor(phase[v]);
			}

			float s = readMorphSample(phase[v], freq);
			float pan = clamp(0.5f + 0.35f * spread, 0.f, 1.f);
			float gainL = std::sqrt(1.f - pan);
			float gainR = std::sqrt(pan);
			outL += s * gainL;
			outR += s * gainR;
		}

		float norm = 1.f / std::sqrt(static_cast<float>(voices));
		outL = clamp(outL * norm * 5.f, -10.f, 10.f);
		outR = clamp(outR * norm * 5.f, -10.f, 10.f);

		float env = computeEnvParam();
		float contourGain = processEnvEnvelope(inputs[TRIG_INPUT].getVoltage(),
		                                       inputs[TRIG_INPUT].isConnected(),
		                                       env,
		                                       args.sampleTime);
		outL *= contourGain;
		outR *= contourGain;

		float rvbMix = clamp(params[RVB_MIX_PARAM].getValue(), 0.f, 1.f);
		if (rvbMix > 1e-4f) {
			float tRaw = clamp(params[RVB_TIME_PARAM].getValue(), 0.f, 1.f);
			float fbRaw = clamp(params[RVB_FB_PARAM].getValue(), 0.f, 1.f);
			float rt60Sec = reverbTimeSecondsFromKnob(tRaw);
			float timeNorm = (rt60Sec - 0.12f) / (10.0f - 0.12f);
			timeNorm = clamp(timeNorm, 0.f, 1.f);

			// ReverbSc behaves best around roughly +/-1 input domain.
			// Rack signals are much hotter, so we scale in/out around the effect.
			const float rackToVerb = 0.20f;
			const float verbToRack = 1.0f / rackToVerb;

			float fbFromTime = 0.60f + 0.38f * std::pow(timeNorm, 0.90f);
			float feedback = clamp(fbFromTime + (fbRaw - 0.5f) * 0.24f, 0.45f, 0.992f);

			float damping = 0.55f * timeNorm + 0.45f * fbRaw;
			float lpHz = 16000.f - 13000.f * std::pow(damping, 0.85f);
			lpHz = clamp(lpHz, 1200.f, 18000.f);

			reverb.SetFeedback(feedback);
			reverb.SetLpFreq(lpHz);

			float dryL = clamp(outL * rackToVerb, -1.2f, 1.2f);
			float dryR = clamp(outR * rackToVerb, -1.2f, 1.2f);
			float wetL = 0.f;
			float wetR = 0.f;
			reverb.Process(dryL, dryR, &wetL, &wetR);
			processReverbWetHighpass(wetL, wetR);

			float wetGain = 1.05f + 0.55f * std::pow(fbRaw, 0.55f);
			wetL *= wetGain;
			wetR *= wetGain;

			float dryMix = std::cos(rvbMix * (0.5f * 3.14159265359f));
			float wetMix = std::sin(rvbMix * (0.5f * 3.14159265359f));
			float mixedL = (dryL * dryMix + wetL * wetMix) * verbToRack;
			float mixedR = (dryR * dryMix + wetR * wetMix) * verbToRack;

			if (!std::isfinite(mixedL) || !std::isfinite(mixedR) ||
			    std::abs(mixedL) > 40.f || std::abs(mixedR) > 40.f) {
				reverb.Init(args.sampleRate > 1000.f ? args.sampleRate : 48000.f);
				outL = dryL * verbToRack;
				outR = dryR * verbToRack;
			}
			else {
				outL = mixedL;
				outR = mixedR;
			}
		}

		outL = sanitizeAudioOut(outL);
		outR = sanitizeAudioOut(outR);

		float blend = tableBlend.load(std::memory_order_relaxed);
		blend = clamp(blend + args.sampleTime / kTableTransitionTimeSec, 0.f, 1.f);
		tableBlend.store(blend, std::memory_order_relaxed);

		outputs[LEFT_OUTPUT].setChannels(1);
		outputs[RIGHT_OUTPUT].setChannels(1);
		outputs[LEFT_OUTPUT].setVoltage(outL);
		outputs[RIGHT_OUTPUT].setVoltage(outR);

		float morphMod = inputs[MORPH_CV_INPUT].isConnected() ? params[MORPH_CV_DEPTH_PARAM].getValue() : 0.f;
		float sizeMod = inputs[WT_SIZE_CV_INPUT].isConnected() ? params[WT_SIZE_CV_DEPTH_PARAM].getValue() : 0.f;
		lights[MORPH_MOD_LIGHT].setBrightnessSmooth(morphMod, args.sampleTime * 12.f);
		lights[WT_SIZE_MOD_LIGHT].setBrightnessSmooth(sizeMod, args.sampleTime * 12.f);
	}
};

struct PanelLabel : TransparentWidget {
	std::string text;
	int fontSize = 8;
	int align = NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE;
	NVGcolor color = nvgRGB(0x0f, 0x17, 0x2a);

	void draw(const DrawArgs& args) override {
		auto font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
		if (!font) {
			return;
		}
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, static_cast<float>(fontSize));
		nvgFillColor(args.vg, color);
		nvgTextAlign(args.vg, align);
		nvgText(args.vg, 0.f, 0.f, text.c_str(), nullptr);
	}
};

struct SourceOverviewDisplay : TransparentWidget {
	SampleVCO* moduleRef = nullptr;

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, 2.5f);
		nvgFillColor(args.vg, nvgRGB(0xec, 0xfe, 0xff));
		nvgFill(args.vg);

		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.5f, 0.5f, box.size.x - 1.f, box.size.y - 1.f, 2.5f);
		nvgStrokeWidth(args.vg, 1.f);
		nvgStrokeColor(args.vg, nvgRGB(0x33, 0x41, 0x55));
		nvgStroke(args.vg);

		if (!moduleRef) {
			return;
		}

		std::array<float, SampleVCO::kMaxWavetableSize> src {};
		int size = 0;
		float windowStartNorm = 0.f;
		float windowSpanNorm = 0.f;
		moduleRef->copySourceOverviewData(src, size, windowStartNorm, windowSpanNorm);
		if (size >= 2) {
			float contentX = 3.f;
			float contentW = box.size.x - 6.f;
			float winX = contentX + clamp(windowStartNorm, 0.f, 1.f) * contentW;
			float winW = clamp(windowSpanNorm, 0.f, 1.f) * contentW;
			winW = clamp(winW, 1.5f, contentW);
			if (winX + winW > contentX + contentW) {
				winX = contentX + contentW - winW;
			}

			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg, winX, 1.5f, winW, box.size.y - 3.f, 1.2f);
			nvgFillColor(args.vg, nvgRGBA(14, 165, 233, 42));
			nvgFill(args.vg);

			nvgBeginPath(args.vg);
			for (int i = 0; i < size; ++i) {
				float t = static_cast<float>(i) / static_cast<float>(size - 1);
				float x = t * contentW + contentX;
				float y = (0.5f - 0.40f * src[i]) * (box.size.y - 6.f) + 3.f;
				if (i == 0) {
					nvgMoveTo(args.vg, x, y);
				}
				else {
					nvgLineTo(args.vg, x, y);
				}
			}
			nvgStrokeWidth(args.vg, 1.1f);
			nvgStrokeColor(args.vg, nvgRGB(0x08, 0x9a, 0xb2));
			nvgStroke(args.vg);

			float markerX = contentX + clamp(windowStartNorm, 0.f, 1.f) * contentW;
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, markerX, 1.5f);
			nvgLineTo(args.vg, markerX, box.size.y - 1.5f);
			nvgStrokeWidth(args.vg, 1.8f);
			nvgStrokeColor(args.vg, nvgRGB(0xef, 0x44, 0x44));
			nvgStroke(args.vg);
		}
	}
};

struct WavetableDisplay : TransparentWidget {
	SampleVCO* moduleRef = nullptr;

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, 3.f);
		nvgFillColor(args.vg, nvgRGB(0xf1, 0xf5, 0xf9));
		nvgFill(args.vg);

		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.5f, 0.5f, box.size.x - 1.f, box.size.y - 1.f, 3.f);
		nvgStrokeWidth(args.vg, 1.f);
		nvgStrokeColor(args.vg, nvgRGB(0x33, 0x41, 0x55));
		nvgStroke(args.vg);

		if (!moduleRef) {
			return;
		}

		std::array<float, SampleVCO::kMaxWavetableSize> wt {};
		int size = 0;
		float scanNorm = 0.f;
		moduleRef->copyDisplayData(wt, size, scanNorm);
		if (size < 2) {
			return;
		}

		nvgBeginPath(args.vg);
		for (int i = 0; i < size; ++i) {
			float t = static_cast<float>(i) / static_cast<float>(size - 1);
			float x = t * (box.size.x - 8.f) + 4.f;
			float y = (0.5f - 0.42f * wt[i]) * (box.size.y - 10.f) + 5.f;
			if (i == 0) {
				nvgMoveTo(args.vg, x, y);
			}
			else {
				nvgLineTo(args.vg, x, y);
			}
		}
		nvgStrokeWidth(args.vg, 1.4f);
		nvgStrokeColor(args.vg, nvgRGB(0x0f, 0x76, 0xbc));
		nvgStroke(args.vg);

		auto font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
		if (font) {
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, 9.f);
			nvgFillColor(args.vg, nvgRGB(0x1f, 0x29, 0x37));
			nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			std::string info = rack::string::f("WT %d  SC %.2f  %s", moduleRef->getPublishedWtSize(), scanNorm,
			                                   moduleRef->getSourceStatusString().c_str());
			nvgText(args.vg, 5.f, 4.f, info.c_str(), nullptr);
		}
	}
};

struct CvDepthKnob : RoundSmallBlackKnob {
	SampleVCO* moduleRef = nullptr;
	int depthParam = -1;
	int cvInput = -1;
	std::string depthMenuLabel = "CV depth";

	void draw(const DrawArgs& args) override {
		RoundSmallBlackKnob::draw(args);
		if (!moduleRef || depthParam < 0 || cvInput < 0) {
			return;
		}
		if (!moduleRef->inputs[cvInput].isConnected()) {
			return;
		}

		auto* pq = getParamQuantity();
		if (!pq) {
			return;
		}

		float minV = pq->getMinValue();
		float maxV = pq->getMaxValue();
		float baseV = moduleRef->params[paramId].getValue();
		float modV = moduleRef->getModulatedKnobValue(baseV, cvInput, depthParam, minV, maxV);
		float depth = clamp(moduleRef->params[depthParam].getValue(), 0.f, 1.f);
		float halfRange = 0.5f * (maxV - minV) * depth;
		float lowV = clamp(baseV - halfRange, minV, maxV);
		float highV = clamp(baseV + halfRange, minV, maxV);

		auto normalize = [minV, maxV](float v) {
			if (maxV <= minV) {
				return 0.f;
			}
			return clamp((v - minV) / (maxV - minV), 0.f, 1.f);
		};

		const float pi = 3.14159265359f;
		const float ringMinA = minAngle - 0.5f * pi;
		const float ringMaxA = maxAngle - 0.5f * pi;
		auto toAngle = [&](float v) {
			float t = normalize(v);
			return ringMinA + t * (ringMaxA - ringMinA);
		};

		const Vec c = box.size.div(2.f);
		const float r = std::min(box.size.x, box.size.y) * 0.56f;

		auto drawArc = [&](float fromValue, float toValue, NVGcolor col, float width) {
			float a0 = toAngle(fromValue);
			float a1 = toAngle(toValue);
			if (a1 < a0) {
				std::swap(a0, a1);
			}
			nvgBeginPath(args.vg);
			nvgArc(args.vg, c.x, c.y, r, a0, a1, NVG_CW);
			nvgStrokeWidth(args.vg, width);
			nvgStrokeColor(args.vg, col);
			nvgStroke(args.vg);
		};

		drawArc(minV, maxV, nvgRGBA(71, 85, 105, 120), 1.2f);
		drawArc(lowV, highV, nvgRGBA(37, 99, 235, 220), 2.0f);

		auto drawTick = [&](float value, NVGcolor col, float width, float len) {
			float a = toAngle(value);
			float x0 = c.x + std::cos(a) * (r - len);
			float y0 = c.y + std::sin(a) * (r - len);
			float x1 = c.x + std::cos(a) * (r + 0.5f);
			float y1 = c.y + std::sin(a) * (r + 0.5f);
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, x0, y0);
			nvgLineTo(args.vg, x1, y1);
			nvgStrokeWidth(args.vg, width);
			nvgStrokeColor(args.vg, col);
			nvgStroke(args.vg);
		};

		drawTick(baseV, nvgRGBA(15, 23, 42, 220), 1.5f, 4.2f);
		drawTick(modV, nvgRGBA(16, 185, 129, 240), 2.2f, 5.8f);
	}

	void appendContextMenu(Menu* menu) override {
		RoundSmallBlackKnob::appendContextMenu(menu);
		if (!moduleRef || depthParam < 0) {
			return;
		}

		int depthRounded = clamp(static_cast<int>(std::round(moduleRef->params[depthParam].getValue() * 100.f)), 0, 100);
		std::string rightText = rack::string::f("%d%%", depthRounded);

		menu->addChild(new MenuSeparator());
		menu->addChild(createSubmenuItem(depthMenuLabel, rightText, [this](Menu* submenu) {
			for (int pct : kDepthMenuSteps) {
				submenu->addChild(createCheckMenuItem(
					rack::string::f("%d%%", pct),
					"",
					[this, pct]() {
						int curr = clamp(static_cast<int>(std::round(moduleRef->params[depthParam].getValue() * 100.f)), 0, 100);
						return curr == pct;
					},
					[this, pct]() {
						moduleRef->params[depthParam].setValue(pct / 100.f);
					}
				));
			}
		}));
	}
};

struct SampleVCOWidget : ModuleWidget {
	SampleVCOWidget(SampleVCO* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/SampleVCO.svg")));

			addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

			auto* sourceDisplay = createWidget<SourceOverviewDisplay>(mm2px(Vec(3.5f, 13.0f)));
			sourceDisplay->box.size = mm2px(Vec(41.5f, 6.0f));
			sourceDisplay->moduleRef = module;
			addChild(sourceDisplay);

			auto* wtDisplay = createWidget<WavetableDisplay>(mm2px(Vec(3.5f, 20.5f)));
			wtDisplay->box.size = mm2px(Vec(41.5f, 18.0f));
			wtDisplay->moduleRef = module;
			addChild(wtDisplay);

		auto* scanKnob = createParamCentered<CvDepthKnob>(mm2px(Vec(52.0f, 16.0f)), module, SampleVCO::MORPH_PARAM);
		scanKnob->moduleRef = module;
		scanKnob->depthParam = SampleVCO::MORPH_CV_DEPTH_PARAM;
		scanKnob->cvInput = SampleVCO::MORPH_CV_INPUT;
		scanKnob->depthMenuLabel = "SCAN CV depth";
		addParam(scanKnob);

		// 16 mm vertical spacing, anchored from the bottom row at y=85.
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(10.0f, 53.0f)), module, SampleVCO::PITCH_PARAM));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(24.0f, 53.0f)), module, SampleVCO::DETUNE_PARAM));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(38.0f, 53.0f)), module, SampleVCO::UNISON_PARAM));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(52.0f, 53.0f)), module, SampleVCO::OCTAVE_PARAM));

			auto* densKnob = createParamCentered<CvDepthKnob>(mm2px(Vec(24.0f, 69.0f)), module, SampleVCO::DENS_PARAM);
			densKnob->moduleRef = module;
			densKnob->depthParam = SampleVCO::DENS_CV_DEPTH_PARAM;
			densKnob->cvInput = SampleVCO::DENS_CV_INPUT;
			densKnob->depthMenuLabel = "DENS CV depth";
			addParam(densKnob);

			auto* smothKnob = createParamCentered<CvDepthKnob>(mm2px(Vec(38.0f, 69.0f)), module, SampleVCO::SMOTH_PARAM);
			smothKnob->moduleRef = module;
			smothKnob->depthParam = SampleVCO::SMOTH_CV_DEPTH_PARAM;
			smothKnob->cvInput = SampleVCO::SMOTH_CV_INPUT;
			smothKnob->depthMenuLabel = "SMOTH CV depth";
			addParam(smothKnob);

			auto* sizeKnob = createParamCentered<CvDepthKnob>(mm2px(Vec(52.0f, 69.0f)), module, SampleVCO::WT_SIZE_PARAM);
			sizeKnob->moduleRef = module;
			sizeKnob->depthParam = SampleVCO::WT_SIZE_CV_DEPTH_PARAM;
			sizeKnob->cvInput = SampleVCO::WT_SIZE_CV_INPUT;
			sizeKnob->depthMenuLabel = "WT SIZE CV depth";
			addParam(sizeKnob);

			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(10.0f, 85.0f)), module, SampleVCO::ENV_PARAM));
			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(24.0f, 85.0f)), module, SampleVCO::RVB_TIME_PARAM));
			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(38.0f, 85.0f)), module, SampleVCO::RVB_FB_PARAM));
			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(52.0f, 85.0f)), module, SampleVCO::RVB_MIX_PARAM));

			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.0f, 101.0f)), module, SampleVCO::MORPH_CV_INPUT));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(24.0f, 101.0f)), module, SampleVCO::DENS_CV_INPUT));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(38.0f, 101.0f)), module, SampleVCO::SMOTH_CV_INPUT));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(52.0f, 101.0f)), module, SampleVCO::WT_SIZE_CV_INPUT));

			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.0f, 117.0f)), module, SampleVCO::VOCT_INPUT));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(24.0f, 117.0f)), module, SampleVCO::TRIG_INPUT));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(38.0f, 117.0f)), module, SampleVCO::LEFT_OUTPUT));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(52.0f, 117.0f)), module, SampleVCO::RIGHT_OUTPUT));

		auto addPanelLabel = [this](float xMm, float yMm, const char* txt, int size = 8, NVGcolor color = nvgRGB(0x0f, 0x17, 0x2a)) {
			auto* l = createWidget<PanelLabel>(mm2px(Vec(xMm, yMm)));
			l->text = txt;
			l->fontSize = size;
			l->color = color;
			addChild(l);
		};

			addPanelLabel(30.5f, 8.0f, "SAMPLE VCO", 10, nvgRGB(0x0b, 0x12, 0x20));
			addPanelLabel(52.0f, 10.0f, "SCAN", 8);

			addPanelLabel(10.0f, 47.0f, "PITCH", 7, nvgRGB(0x1f, 0x29, 0x37));
			addPanelLabel(24.0f, 47.0f, "DETUNE", 7, nvgRGB(0x1f, 0x29, 0x37));
			addPanelLabel(38.0f, 47.0f, "UNISON", 7, nvgRGB(0x1f, 0x29, 0x37));
			addPanelLabel(52.0f, 47.0f, "OCT", 7, nvgRGB(0x1f, 0x29, 0x37));

			addPanelLabel(24.0f, 63.0f, "DENS", 8);
			addPanelLabel(38.0f, 63.0f, "SMOTH", 8);
			addPanelLabel(52.0f, 63.0f, "WTSIZE", 8);

			addPanelLabel(10.0f, 79.0f, "ENV", 8);
			addPanelLabel(24.0f, 79.0f, "RVB TM", 8);
			addPanelLabel(38.0f, 79.0f, "RVB FB", 8);
			addPanelLabel(52.0f, 79.0f, "RVB MIX", 8);

			addPanelLabel(10.0f, 95.0f, "SCAN CV", 7);
			addPanelLabel(24.0f, 95.0f, "DENS CV", 7);
			addPanelLabel(38.0f, 95.0f, "SMOTH CV", 7);
			addPanelLabel(52.0f, 95.0f, "WT CV", 7);

			addPanelLabel(10.0f, 111.0f, "VOCT", 7);
			addPanelLabel(24.0f, 111.0f, "TRIG", 7);
			addPanelLabel(38.0f, 111.0f, "L OUT", 7);
			addPanelLabel(52.0f, 111.0f, "R OUT", 7);
		}

	void appendContextMenu(Menu* menu) override {
		ModuleWidget::appendContextMenu(menu);
		auto* m = dynamic_cast<SampleVCO*>(module);
		if (!m) {
			return;
		}

		menu->addChild(new MenuSeparator());
		menu->addChild(createMenuLabel("WAV source (first 5s)"));
		menu->addChild(createMenuItem("Load WAV...", "", [m]() {
			char* pathC = osdialog_file(OSDIALOG_OPEN, nullptr, nullptr, nullptr);
			if (!pathC) {
				return;
			}
			std::string path(pathC);
			std::free(pathC);
			m->loadSourceWavPath(path);
		}));
		menu->addChild(createMenuItem("Clear WAV", "", [m]() {
			m->clearSourceWav();
		}));
		menu->addChild(createMenuLabel(m->getSourceStatusString()));
	}
};

Model* modelWaveFileVCO = createModel<SampleVCO, SampleVCOWidget>("WaveFileVCO");
