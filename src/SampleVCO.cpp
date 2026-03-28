#include "plugin.hpp"
#include "reverbsc.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <string>
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

struct CloudsStyleReverb {
	static constexpr int kMemorySize = 16384;
	static constexpr int kMemoryMask = kMemorySize - 1;
	static constexpr float kTwoPi = 6.28318530718f;

	struct DelayLineSpec {
		int base;
		int length;
	};

	const DelayLineSpec ap1 {0, 113};
	const DelayLineSpec ap2 {114, 162};
	const DelayLineSpec ap3 {277, 241};
	const DelayLineSpec ap4 {519, 399};
	const DelayLineSpec dap1a {919, 1653};
	const DelayLineSpec dap1b {2573, 2038};
	const DelayLineSpec del1 {4612, 3411};
	const DelayLineSpec dap2a {8024, 1913};
	const DelayLineSpec dap2b {9938, 1663};
	const DelayLineSpec del2 {11602, 4782};

	std::array<float, kMemorySize> memory {};
	int writePtr = 0;
	float sampleRate = 48000.f;

	float lfoPhase1 = 0.f;
	float lfoPhase2 = 0.f;
	float lfoFreq1 = 0.5f;
	float lfoFreq2 = 0.3f;

	float amount = 0.f;
	float inputGain = 0.25f;
	float reverbTime = 0.7f;
	float diffusion = 0.625f;
	float lp = 0.7f;
	float lpDecay1 = 0.f;
	float lpDecay2 = 0.f;

	void init(float sr) {
		sampleRate = std::max(sr, 1000.f);
		clear();
	}

	void clear() {
		memory.fill(0.f);
		writePtr = 0;
		lfoPhase1 = 0.f;
		lfoPhase2 = 0.f;
		lpDecay1 = 0.f;
		lpDecay2 = 0.f;
	}

	void set_amount(float v) {
		amount = clamp(v, 0.f, 1.f);
	}

	void set_input_gain(float v) {
		inputGain = clamp(v, 0.f, 1.f);
	}

	void set_time(float v) {
		reverbTime = clamp(v, 0.f, 0.995f);
	}

	void set_diffusion(float v) {
		diffusion = clamp(v, 0.f, 0.95f);
	}

	void set_lp(float v) {
		lp = clamp(v, 0.f, 0.999f);
	}

	int delayIndex(const DelayLineSpec& d, int offset) const {
		int off = offset;
		if (off < 0) {
			off = d.length - 1;
		}
		else if (off >= d.length) {
			off = d.length - 1;
		}
		return (writePtr + d.base + off) & kMemoryMask;
	}

	float delayRead(const DelayLineSpec& d, int offset) const {
		return memory[delayIndex(d, offset)];
	}

	float delayReadInterp(const DelayLineSpec& d, float offset) const {
		float o = clamp(offset, 0.f, static_cast<float>(d.length - 2));
		int oi = static_cast<int>(std::floor(o));
		float frac = o - static_cast<float>(oi);
		float a = memory[(writePtr + d.base + oi) & kMemoryMask];
		float b = memory[(writePtr + d.base + oi + 1) & kMemoryMask];
		return a + (b - a) * frac;
	}

	void delayWrite(const DelayLineSpec& d, int offset, float value) {
		if (!std::isfinite(value)) {
			value = 0.f;
		}
		// Clouds stores internal reverb state with much larger headroom than +/-1.
		// Keep generous range to avoid premature clipping/alias-like hash.
		memory[delayIndex(d, offset)] = clamp(value, -8.f, 8.f);
	}

	void tickLfos() {
		float f1 = lfoFreq1 / sampleRate;
		float f2 = lfoFreq2 / sampleRate;
		lfoPhase1 += f1;
		lfoPhase2 += f2;
		if (lfoPhase1 >= 1.f) {
			lfoPhase1 -= 1.f;
		}
		if (lfoPhase2 >= 1.f) {
			lfoPhase2 -= 1.f;
		}
	}

	void process(float& inOutL, float& inOutR) {
		if (amount <= 1e-4f) {
			return;
		}

		--writePtr;
		if (writePtr < 0) {
			writePtr += kMemorySize;
		}
		tickLfos();

		float lfo1 = std::cos(kTwoPi * lfoPhase1);
		float lfo2 = std::cos(kTwoPi * lfoPhase2);

		const float kap = diffusion;
		const float klp = lp;
		const float krt = reverbTime;
		const float gain = inputGain;

		float apout = 0.f;
		float wet = 0.f;
		float lp1 = lpDecay1;
		float lp2 = lpDecay2;

		auto allpassClouds = [this](const DelayLineSpec& d,
		                            float x,
		                            float readScale,
		                            float writeScale,
		                            int writeOffset = 0) {
			float delayed = delayRead(d, -1);
			float acc = x + delayed * readScale;
			delayWrite(d, writeOffset, acc);
			return acc * writeScale + delayed;
		};

		float acc = delayReadInterp(ap1, 10.f + lfo1 * 60.f);
		delayWrite(ap1, 100, acc);
		acc = (inOutL + inOutR) * gain + acc;

		acc = allpassClouds(ap1, acc, kap, -kap);
		acc = allpassClouds(ap2, acc, kap, -kap);
		acc = allpassClouds(ap3, acc, kap, -kap);
		acc = allpassClouds(ap4, acc, kap, -kap);
		apout = acc;

		acc = apout + delayReadInterp(del2, 4680.f + lfo2 * 100.f) * krt;
		lp1 += klp * (acc - lp1);
		acc = lp1;
		acc = allpassClouds(dap1a, acc, -kap, kap);
		acc = allpassClouds(dap1b, acc, kap, -kap);
		delayWrite(del1, 0, acc);
		acc *= 2.f;
		wet = acc;
		inOutL += (wet - inOutL) * amount;

		acc = apout + delayRead(del1, -1) * krt;
		lp2 += klp * (acc - lp2);
		acc = lp2;
		acc = allpassClouds(dap2a, acc, kap, -kap);
		acc = allpassClouds(dap2b, acc, -kap, kap);
		delayWrite(del2, 0, acc);
		acc *= 2.f;
		wet = acc;
		inOutR += (wet - inOutR) * amount;

		lpDecay1 = lp1;
		lpDecay2 = lp2;

		inOutL = clamp(inOutL, -10.f, 10.f);
		inOutR = clamp(inOutR, -10.f, 10.f);
	}
};

struct NoiseVCO : Module {
	static constexpr int kMaxWavetableSize = 4096;
	static constexpr int kMaxVoices = 10; // 1 + unison(0..9)
	static constexpr int kGeneratedWavetableSize = 2048;
	static constexpr int kMipLevels = 5; // 2048,1024,512,256,128
	static constexpr int kMaxDensePoints = 48;
	static constexpr int kMaxAnchorPoints = kMaxDensePoints + 2;
	static constexpr float kTableTransitionTimeSec = 0.5f; // 500 ms
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
		GEN_PARAM,
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
		GEN_TRIG_INPUT,
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
		GEN_LIGHT,
		MORPH_MOD_LIGHT,
		WT_SIZE_MOD_LIGHT,
		NUM_LIGHTS
	};

	dsp::SchmittTrigger genButtonTrigger;
	dsp::SchmittTrigger genInputTrigger;
	dsp::SchmittTrigger contourTrigger;
	dsp::PulseGenerator genLightPulse;
	std::mt19937 rng {0x4e565f43u};

	std::array<float, kMaxWavetableSize> wtA {};
	std::array<float, kMaxWavetableSize> wtB {};
	// Base morph table (A<->B) always rendered at fixed 2048 samples.
	std::array<float, kMaxWavetableSize> wtMorphBase {};
	// Active playback table after WT SIZE crop+resample+windowing.
	std::array<float, kMaxWavetableSize> wtMorph {};
	// Previous playback table for output/display crossfade.
	std::array<float, kMaxWavetableSize> wtMorphPrev {};
	// Band-limited mipmap pyramids for current and previous tables.
	std::array<std::array<float, kGeneratedWavetableSize>, kMipLevels> wtMorphMip {};
	std::array<std::array<float, kGeneratedWavetableSize>, kMipLevels> wtMorphPrevMip {};
	std::array<int, kMipLevels> wtMorphMipSize {};
	std::array<int, kMipLevels> wtMorphPrevMipSize {};
	int wtSize = 1024;
	float lastMorph = -1.f;
	int lastDense = 10;
	int lastSmoth = 0; // 0..100
	float tableBlend = 1.f;
	bool pendingGenRequest = false;

	std::array<int, kMaxAnchorPoints> wtAnchorIndex {};
	std::array<float, kMaxAnchorPoints> wtAnchorA {};
	std::array<float, kMaxAnchorPoints> wtAnchorB {};
	int wtAnchorCount = 0;
	std::array<int, kMaxDensePoints> denseMemoryIndex {};
	std::array<float, kMaxDensePoints> denseMemoryA {};
	std::array<float, kMaxDensePoints> denseMemoryB {};

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
	std::vector<float> sourceMono;
	float sourceSampleRate = 0.f;
	std::string sourcePath;
	std::string sourceError;
	bool sourceLoaded = false;

	mutable std::mutex wtMutex;

	NoiseVCO() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

		configInput(VOCT_INPUT, "V/Oct");
		configInput(TRIG_INPUT, "Trigger");
		configInput(GEN_TRIG_INPUT, "Jump trigger");
		configInput(MORPH_CV_INPUT, "Morph CV");
		configInput(WT_SIZE_CV_INPUT, "WT Size CV");
		configInput(DENS_CV_INPUT, "Scan CV");
		configInput(SMOTH_CV_INPUT, "Span CV");

		configOutput(LEFT_OUTPUT, "Left");
		configOutput(RIGHT_OUTPUT, "Right");

		configParam<PitchLikeSurgeQuantity>(PITCH_PARAM, -5.f, 5.f, 0.f, "Pitch (v/oct)");
		configParam(DETUNE_PARAM, 0.f, 12.f, 0.2f, "Detune", " semitones");
		auto* unisonQ = configParam(UNISON_PARAM, 0.f, 9.f, 0.f, "Unison");
		unisonQ->snapEnabled = true;
		auto* octaveQ = configParam(OCTAVE_PARAM, -3.f, 3.f, 0.f, "Octave shift", " oct");
		octaveQ->snapEnabled = true;
		configParam(MORPH_PARAM, 0.f, 1.f, 0.5f, "Morph");
		auto* wtSizeQ = configParam(WT_SIZE_PARAM, 256.f, 2048.f, 1024.f, "WT size");
		wtSizeQ->snapEnabled = true;
		auto* densQ = configParam(DENS_PARAM, 1.f, 48.f, 24.f, "Scan");
		densQ->snapEnabled = true;
		auto* smothQ = configParam(SMOTH_PARAM, 0.f, 100.f, 20.f, "Span / smooth");
		smothQ->snapEnabled = true;
		configButton(GEN_PARAM, "Jump");
		configParam(ENV_PARAM, 0.f, 1.f, 1.f, "Envelope");
		configParam<ReverbTimeSecondsQuantity>(RVB_TIME_PARAM, 0.f, 1.f, 0.4f, "Reverb time");
		configParam(RVB_FB_PARAM, 0.f, 1.f, 0.45f, "Reverb feedback");
		configParam(RVB_MIX_PARAM, 0.f, 1.f, 0.f, "Reverb mix");

		configParam(MORPH_CV_DEPTH_PARAM, 0.f, 1.f, 1.f, "Morph CV depth", "%", 0.f, 100.f);
		configParam(WT_SIZE_CV_DEPTH_PARAM, 0.f, 1.f, 1.f, "WT Size CV depth", "%", 0.f, 100.f);
		configParam(DENS_CV_DEPTH_PARAM, 0.f, 1.f, 1.f, "Scan CV depth", "%", 0.f, 100.f);
		configParam(SMOTH_CV_DEPTH_PARAM, 0.f, 1.f, 1.f, "Span CV depth", "%", 0.f, 100.f);

		buildDenseMemoryIndices();
		regenerateNoiseSources(false);
		regenerateWavePair(computeDenseParam(), computeSmothParam());
		rebuildMorphBaseTable(computeMorphParam());
		rebuildPlaybackTable(wtSize);
		wtMorphPrev = wtMorph;
		wtMorphPrevMip = wtMorphMip;
		wtMorphPrevMipSize = wtMorphMipSize;
		reverb.Init(48000.f);
		updateReverbWetHighpass(48000.f);
		resetReverbWetHighpass();
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

		float curr0 = readWavetableLevelSample(wtMorphMip, wtMorphMipSize, level0, ph);
		float curr1 = readWavetableLevelSample(wtMorphMip, wtMorphMipSize, level1, ph);
		float curr = sanitizeWaveSample(curr0 + (curr1 - curr0) * levelBlend);

		float prev0 = readWavetableLevelSample(wtMorphPrevMip, wtMorphPrevMipSize, level0, ph);
		float prev1 = readWavetableLevelSample(wtMorphPrevMip, wtMorphPrevMipSize, level1, ph);
		float prev = sanitizeWaveSample(prev0 + (prev1 - prev0) * levelBlend);

		return sanitizeWaveSample(prev + (curr - prev) * tableBlend);
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

	float computeMorphParam() {
		return getModulatedKnobValue(params[MORPH_PARAM].getValue(), MORPH_CV_INPUT, MORPH_CV_DEPTH_PARAM, 0.f, 1.f);
	}

	bool loadSourceWavPath(const std::string& path) {
		std::vector<float> mono;
		float sr = 0.f;
		std::string err;
		if (!loadWavMonoLimited5s(path, mono, sr, err)) {
			std::lock_guard<std::mutex> lock(wtMutex);
			sourceLoaded = false;
			sourceMono.clear();
			sourceSampleRate = 0.f;
			sourcePath.clear();
			sourceError = err;
			pendingGenRequest = true;
			return false;
		}

		std::lock_guard<std::mutex> lock(wtMutex);
		sourceLoaded = true;
		sourceMono = std::move(mono);
		sourceSampleRate = sr;
		sourcePath = path;
		sourceError.clear();
		pendingGenRequest = true;
		return true;
	}

	void clearSourceWav() {
		std::lock_guard<std::mutex> lock(wtMutex);
		sourceLoaded = false;
		sourceMono.clear();
		sourceSampleRate = 0.f;
		sourcePath.clear();
		sourceError.clear();
		pendingGenRequest = true;
	}

	std::string getSourceStatusString() const {
		std::lock_guard<std::mutex> lock(wtMutex);
		if (sourceLoaded && sourceSampleRate > 1000.f) {
			float sec = static_cast<float>(sourceMono.size()) / sourceSampleRate;
			return rack::string::f("WAV %.2fs", sec);
		}
		if (!sourceError.empty()) {
			return std::string("WAV ERR: ") + sourceError;
		}
		return "RANDOM SOURCE";
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
		                                DENS_CV_DEPTH_PARAM, 1.f, 48.f);
		return clamp(static_cast<int>(std::round(v)), 1, 48);
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

	// Legacy algorithm kept for reference and potential A/B tests.
	// Summary: overscanned noise -> zero-cross search -> resample to table size -> normalize.
	void generateNoiseWindowedWavetableLegacy(std::array<float, kMaxWavetableSize>& outTable, int size) {
		const int overscan = 4;
		const int noiseLen = std::max(size * overscan, size + 2);
		std::uniform_real_distribution<float> dist(-1.f, 1.f);

		std::vector<float> noise;
		noise.reserve(noiseLen);
		for (int i = 0; i < noiseLen; ++i) {
			noise.push_back(dist(rng));
		}

		std::vector<int> crossings;
		crossings.reserve(noiseLen / 4);
		for (int i = 0; i < noiseLen - 1; ++i) {
			float a = noise[i];
			float b = noise[i + 1];
			if ((a <= 0.f && b > 0.f) || (a >= 0.f && b < 0.f)) {
				crossings.push_back(i);
			}
		}

		int start = 0;
		int end = std::min(noiseLen - 1, size);
		if (crossings.size() >= 2) {
			start = crossings.front();
			int preferredLen = size;
			int minLen = std::max(8, size / 2);
			int bestEnd = -1;
			int bestScore = std::numeric_limits<int>::max();
			for (int c : crossings) {
				int len = c - start;
				if (len < minLen) {
					continue;
				}
				int score = std::abs(len - preferredLen);
				if (score < bestScore) {
					bestScore = score;
					bestEnd = c;
				}
			}
			if (bestEnd > start + 1) {
				end = bestEnd;
			}
		}

		int segmentLen = std::max(2, end - start);
		for (int i = 0; i < size; ++i) {
			float pos = (static_cast<float>(i) / static_cast<float>(size - 1)) * static_cast<float>(segmentLen - 1);
			int i0 = static_cast<int>(std::floor(pos));
			int i1 = std::min(i0 + 1, segmentLen - 1);
			float frac = pos - static_cast<float>(i0);
			int n0 = clamp(start + i0, 0, noiseLen - 1);
			int n1 = clamp(start + i1, 0, noiseLen - 1);
			outTable[i] = noise[n0] + (noise[n1] - noise[n0]) * frac;
		}

		float peak = 1e-6f;
		for (int i = 0; i < size; ++i) {
			peak = std::max(peak, std::abs(outTable[i]));
		}
		float invPeak = 1.f / peak;
		for (int i = 0; i < size; ++i) {
			outTable[i] = sanitizeWaveSample(outTable[i] * invPeak);
		}
	}

	void buildDenseMemoryIndices() {
		int prev = 0;
		for (int i = 0; i < kMaxDensePoints; ++i) {
			float t = static_cast<float>(i + 1) / static_cast<float>(kMaxDensePoints + 1);
			int idx = static_cast<int>(std::lround(t * static_cast<float>(kGeneratedWavetableSize - 1)));
			int remaining = kMaxDensePoints - 1 - i;
			int maxAllowed = (kGeneratedWavetableSize - 2) - remaining;
			idx = clamp(idx, prev, maxAllowed);
			denseMemoryIndex[i] = idx;
			prev = idx + 1;
		}
	}

	void buildDenseAnchorIndices(int dens) {
		int interiorCount = std::max(1, std::min(dens, kMaxDensePoints));
		wtAnchorCount = interiorCount + 2;
		wtAnchorIndex[0] = 0;
		wtAnchorA[0] = 0.f;
		wtAnchorB[0] = 0.f;

		int prevMemory = -1;
		for (int i = 0; i < interiorCount; ++i) {
			float t = static_cast<float>(i + 1) / static_cast<float>(interiorCount + 1);
			int mem = static_cast<int>(std::lround(t * static_cast<float>(kMaxDensePoints + 1))) - 1;
			mem = clamp(mem, 0, kMaxDensePoints - 1);

			int remaining = interiorCount - 1 - i;
			int maxAllowed = (kMaxDensePoints - 1) - remaining;
			mem = clamp(mem, prevMemory + 1, maxAllowed);
			prevMemory = mem;

			wtAnchorIndex[i + 1] = denseMemoryIndex[mem];
			wtAnchorA[i + 1] = denseMemoryA[mem];
			wtAnchorB[i + 1] = denseMemoryB[mem];
		}
		wtAnchorIndex[wtAnchorCount - 1] = kGeneratedWavetableSize - 1;
		wtAnchorA[wtAnchorCount - 1] = 0.f;
		wtAnchorB[wtAnchorCount - 1] = 0.f;
	}

	void regenerateNoiseSources(bool randomizeFromTrigger = false) {
		std::lock_guard<std::mutex> lock(wtMutex);
		if (sourceLoaded && sourceSampleRate > 1000.f && sourceMono.size() > 1024) {
			int totalFrames = static_cast<int>(sourceMono.size());
			int windowFrames = clamp(computeWavetableSize() * 2, 256, 8192);
			windowFrames = std::min(windowFrames, std::max(64, totalFrames - 1));
			int maxStart = std::max(0, totalFrames - windowFrames - 1);

			float scan = (computeDenseParam() - 1.f) / 47.f;
			float span = std::pow(computeSmothParam() * 0.01f, 1.2f) * 0.35f;
			if (randomizeFromTrigger) {
				std::uniform_real_distribution<float> jitterDist(-0.18f, 0.18f);
				scan = clamp(scan + jitterDist(rng), 0.f, 1.f);
			}

			float startANorm = clamp(scan - 0.5f * span, 0.f, 1.f);
			float startBNorm = clamp(scan + 0.5f * span, 0.f, 1.f);
			int startA = static_cast<int>(std::lround(startANorm * static_cast<float>(maxStart)));
			int startB = static_cast<int>(std::lround(startBNorm * static_cast<float>(maxStart)));

			for (int i = 0; i < kMaxDensePoints; ++i) {
				float t = static_cast<float>(i + 1) / static_cast<float>(kMaxDensePoints + 1);
				int off = static_cast<int>(std::lround(t * static_cast<float>(windowFrames - 1)));
				int ia = clamp(startA + off, 0, totalFrames - 1);
				int ib = clamp(startB + off, 0, totalFrames - 1);
				denseMemoryA[i] = sanitizeWaveSample(sourceMono[ia]);
				denseMemoryB[i] = sanitizeWaveSample(sourceMono[ib]);
			}
			return;
		}

		std::uniform_real_distribution<float> dist(-1.f, 1.f);
		for (int i = 0; i < kMaxDensePoints; ++i) {
			denseMemoryA[i] = dist(rng);
			denseMemoryB[i] = dist(rng);
		}
	}

	void smoothAnchorsInPlace(std::array<float, kMaxAnchorPoints>& anchors, float smoth) {
		if (wtAnchorCount <= 3 || smoth <= 0.f) {
			return;
		}

		float sm = clamp(smoth, 0.f, 1.f);
		std::array<float, kMaxAnchorPoints> original = anchors;
		int interiorMax = wtAnchorCount - 2;
		int radius = 1 + static_cast<int>(std::round(sm * 10.f));
		radius = clamp(radius, 1, std::max(1, interiorMax - 1));
		float sigma = std::max(0.5f, 0.35f * static_cast<float>(radius));
		float twoSigma2 = 2.f * sigma * sigma;

		for (int i = 1; i <= interiorMax; ++i) {
			int from = std::max(1, i - radius);
			int to = std::min(interiorMax, i + radius);
			float sumW = 0.f;
			float sumV = 0.f;
			for (int j = from; j <= to; ++j) {
				float d = static_cast<float>(std::abs(j - i));
				float w = std::exp(-(d * d) / twoSigma2);
				sumW += w;
				sumV += w * original[j];
			}

			float filtered = (sumW > 1e-9f) ? (sumV / sumW) : original[i];
			anchors[i] = sanitizeWaveSample(original[i] + (filtered - original[i]) * sm);
		}

		anchors[0] = 0.f;
		anchors[wtAnchorCount - 1] = 0.f;
	}

	void renderDenseAnchorsToTable(const std::array<float, kMaxAnchorPoints>& anchors, int size,
	                               float smoth, std::array<float, kMaxWavetableSize>& outTable) {
		const float pi = 3.14159265359f;
		float sm = clamp(smoth, 0.f, 1.f);
		for (int seg = 0; seg < wtAnchorCount - 1; ++seg) {
			int i0 = wtAnchorIndex[seg];
			int i1 = wtAnchorIndex[seg + 1];
			float y0 = anchors[seg];
			float y1 = anchors[seg + 1];
			int span = std::max(1, i1 - i0);

			for (int i = i0; i <= i1; ++i) {
				float t = static_cast<float>(i - i0) / static_cast<float>(span);
				float linear = y0 + (y1 - y0) * t;
				float smoothT = 0.5f - 0.5f * std::cos(pi * t);
				float curved = y0 + (y1 - y0) * smoothT;
				outTable[i] = linear + (curved - linear) * sm;
			}
		}

		// Hard constraints from spec.
		outTable[0] = 0.f;
		outTable[size - 1] = 0.f;

		float peak = 1e-6f;
		for (int i = 0; i < size; ++i) {
			peak = std::max(peak, std::abs(outTable[i]));
		}
		float invPeak = 1.f / peak;
		for (int i = 0; i < size; ++i) {
			outTable[i] = sanitizeWaveSample(outTable[i] * invPeak);
		}
		outTable[0] = 0.f;
		outTable[size - 1] = 0.f;
	}

	void regenerateWavePair(int dens, int smoth) {
		buildDenseAnchorIndices(dens);
		lastDense = dens;

		std::lock_guard<std::mutex> lock(wtMutex);
		float smothF = clamp(smoth * 0.01f, 0.f, 1.f);
		smoothAnchorsInPlace(wtAnchorA, smothF);
		smoothAnchorsInPlace(wtAnchorB, smothF);
		renderDenseAnchorsToTable(wtAnchorA, kGeneratedWavetableSize, smothF, wtA);
		renderDenseAnchorsToTable(wtAnchorB, kGeneratedWavetableSize, smothF, wtB);
		lastSmoth = smoth;
	}

	void rebuildMorphBaseTable(float morph) {
		int sizeLocal = kGeneratedWavetableSize;
		std::lock_guard<std::mutex> lock(wtMutex);
		for (int i = 0; i < sizeLocal; ++i) {
			wtMorphBase[i] = sanitizeWaveSample(wtA[i] + (wtB[i] - wtA[i]) * morph);
		}
		lastMorph = morph;
	}

	void rebuildMipmapsFromTable(
		const std::array<float, kMaxWavetableSize>& source,
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

	void rebuildPlaybackTable(int windowSize) {
		const int outSize = kGeneratedWavetableSize;
		const int localWtSize = clamp(windowSize, 256, kGeneratedWavetableSize);
		const float outDenom = static_cast<float>(outSize - 1);
		const float srcDenom = static_cast<float>(localWtSize - 1);
		const float pi = 3.14159265359f;

		std::lock_guard<std::mutex> lock(wtMutex);
		for (int i = 0; i < outSize; ++i) {
			float t = static_cast<float>(i) / outDenom;
			float srcPos = t * srcDenom;
			int i0 = static_cast<int>(std::floor(srcPos));
			int i1 = std::min(i0 + 1, localWtSize - 1);
			float frac = srcPos - static_cast<float>(i0);
			float s0 = wtMorphBase[i0];
			float s1 = wtMorphBase[i1];
			wtMorph[i] = sanitizeWaveSample(s0 + (s1 - s0) * frac);
		}

		// Apply symmetric cosine edge window so start/end stay stable at WT SIZE changes.
		const int edgeSamples = 64;
		const int edge = std::min(edgeSamples, outSize / 2);
		for (int i = 0; i < edge; ++i) {
				float t = static_cast<float>(i) / static_cast<float>(std::max(1, edge - 1));
				float w = 0.5f - 0.5f * std::cos(pi * t);
				wtMorph[i] = sanitizeWaveSample(wtMorph[i] * w);
				wtMorph[outSize - 1 - i] = sanitizeWaveSample(wtMorph[outSize - 1 - i] * w);
			}

		wtMorph[0] = 0.f;
		wtMorph[outSize - 1] = 0.f;
		wtSize = localWtSize;
		rebuildMipmapsFromTable(wtMorph, wtMorphMip, wtMorphMipSize);
	}

	void copyDisplayData(std::array<float, kMaxWavetableSize>& outData, int& outSize, float& outMorph) const {
		std::lock_guard<std::mutex> lock(wtMutex);
		outSize = kGeneratedWavetableSize;
		float blend = clamp(tableBlend, 0.f, 1.f);
		for (int i = 0; i < outSize; ++i) {
			float prev = wtMorphPrev[i];
			float curr = wtMorph[i];
			outData[i] = sanitizeWaveSample(prev + (curr - prev) * blend);
		}
		outMorph = lastMorph;
	}

	void captureAudibleMorphTable(std::array<float, kMaxWavetableSize>& outData) {
		const int outSize = kGeneratedWavetableSize;
		float blend = clamp(tableBlend, 0.f, 1.f);
		for (int i = 0; i < outSize; ++i) {
			float prev = wtMorphPrev[i];
			float curr = wtMorph[i];
			outData[i] = sanitizeWaveSample(prev + (curr - prev) * blend);
		}
		outData[0] = 0.f;
		outData[outSize - 1] = 0.f;
	}

	void onReset() override {
		for (float& p : phase) {
			p = 0.f;
		}
		buildDenseMemoryIndices();
		regenerateNoiseSources(false);
		regenerateWavePair(computeDenseParam(), computeSmothParam());
		rebuildMorphBaseTable(0.5f);
		rebuildPlaybackTable(computeWavetableSize());
		wtMorphPrev = wtMorph;
		wtMorphPrevMip = wtMorphMip;
		wtMorphPrevMipSize = wtMorphMipSize;
		tableBlend = 1.f;
		pendingGenRequest = false;
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
			std::lock_guard<std::mutex> lock(wtMutex);
			if (sourceLoaded && !sourcePath.empty()) {
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
		int targetSize = computeWavetableSize();
		int targetDense = computeDenseParam();
		int targetSmoth = computeSmothParam();
		float targetMorph = computeMorphParam();

		bool needSeed = pendingGenRequest;
		bool sizeChanged = targetSize != wtSize;
		bool waveShapeChanged = needSeed || (targetDense != lastDense) || (targetSmoth != lastSmoth) ||
		                        (wtAnchorCount < 3);
		bool morphChanged = std::abs(targetMorph - lastMorph) > 1e-4f;

		if (sizeChanged || waveShapeChanged) {
			captureAudibleMorphTable(wtMorphPrev);
			rebuildMipmapsFromTable(wtMorphPrev, wtMorphPrevMip, wtMorphPrevMipSize);
			if (waveShapeChanged) {
				if (needSeed) {
					regenerateNoiseSources(needSeed);
				}
				regenerateWavePair(targetDense, targetSmoth);
			}
			if (waveShapeChanged || morphChanged) {
				rebuildMorphBaseTable(targetMorph);
			}
			rebuildPlaybackTable(targetSize);
			tableBlend = 0.f;
		}
		else if (morphChanged) {
			rebuildMorphBaseTable(targetMorph);
			rebuildPlaybackTable(wtSize);
		}

		if (needSeed) {
			genLightPulse.trigger(0.08f);
		}
		pendingGenRequest = false;
	}

	void process(const ProcessArgs& args) override {
		bool genEvent = false;
		if (genButtonTrigger.process(params[GEN_PARAM].getValue())) {
			genEvent = true;
		}
		if (genInputTrigger.process(inputs[GEN_TRIG_INPUT].getVoltage())) {
			genEvent = true;
		}
		if (genEvent) {
			pendingGenRequest = true;
		}

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

		tableBlend = clamp(tableBlend + args.sampleTime / kTableTransitionTimeSec, 0.f, 1.f);

		outputs[LEFT_OUTPUT].setChannels(1);
		outputs[RIGHT_OUTPUT].setChannels(1);
		outputs[LEFT_OUTPUT].setVoltage(outL);
		outputs[RIGHT_OUTPUT].setVoltage(outR);

		float genLit = genLightPulse.process(args.sampleTime) ? 1.f : 0.f;
		lights[GEN_LIGHT].setBrightness(genLit);

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

struct PanelGridOverlay : TransparentWidget {
	float xStartMm = 10.f;
	float xStepMm = 14.f;
	float yStartMm = 15.f;
	float yStepMm = 16.f;

	void draw(const DrawArgs& args) override {
		float xStartPx = mm2px(Vec(xStartMm, 0.f)).x;
		float xStepPx = mm2px(Vec(xStepMm, 0.f)).x;
		float yStartPx = mm2px(Vec(0.f, yStartMm)).y;
		float yStepPx = mm2px(Vec(0.f, yStepMm)).y;

		nvgBeginPath(args.vg);
		for (float x = xStartPx; x <= box.size.x; x += xStepPx) {
			nvgMoveTo(args.vg, x, 0.f);
			nvgLineTo(args.vg, x, box.size.y);
		}
		for (float y = yStartPx; y <= box.size.y; y += yStepPx) {
			nvgMoveTo(args.vg, 0.f, y);
			nvgLineTo(args.vg, box.size.x, y);
		}
		nvgStrokeWidth(args.vg, 0.8f);
		nvgStrokeColor(args.vg, nvgRGBA(15, 23, 42, 45));
		nvgStroke(args.vg);

		// Highlight raster origin.
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, xStartPx, 0.f);
		nvgLineTo(args.vg, xStartPx, box.size.y);
		nvgMoveTo(args.vg, 0.f, yStartPx);
		nvgLineTo(args.vg, box.size.x, yStartPx);
		nvgStrokeWidth(args.vg, 1.0f);
		nvgStrokeColor(args.vg, nvgRGBA(15, 23, 42, 85));
		nvgStroke(args.vg);
	}
};

struct WavetableDisplay : TransparentWidget {
	NoiseVCO* moduleRef = nullptr;

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

		std::array<float, NoiseVCO::kMaxWavetableSize> wt {};
		int size = 0;
		float morph = 0.f;
		moduleRef->copyDisplayData(wt, size, morph);
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
			std::string info = rack::string::f("WT %d  M %.2f  %s", moduleRef->wtSize, morph,
			                                   moduleRef->getSourceStatusString().c_str());
			nvgText(args.vg, 5.f, 4.f, info.c_str(), nullptr);
		}
	}
};

struct CvDepthKnob : RoundSmallBlackKnob {
	NoiseVCO* moduleRef = nullptr;
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

struct NoiseVCOWidget : ModuleWidget {
	NoiseVCOWidget(NoiseVCO* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/NoiseVCO.svg")));

			addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

			auto* display = createWidget<WavetableDisplay>(mm2px(Vec(3.5f, 12.0f)));
			display->box.size = mm2px(Vec(41.5f, 24.0f));
			display->moduleRef = module;
			addChild(display);

		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(10.0f, 47.0f)), module, NoiseVCO::PITCH_PARAM));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(24.0f, 47.0f)), module, NoiseVCO::DETUNE_PARAM));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(38.0f, 47.0f)), module, NoiseVCO::UNISON_PARAM));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(52.0f, 47.0f)), module, NoiseVCO::OCTAVE_PARAM));

		auto* morphKnob = createParamCentered<CvDepthKnob>(mm2px(Vec(10.0f, 63.0f)), module, NoiseVCO::MORPH_PARAM);
		morphKnob->moduleRef = module;
		morphKnob->depthParam = NoiseVCO::MORPH_CV_DEPTH_PARAM;
		morphKnob->cvInput = NoiseVCO::MORPH_CV_INPUT;
		morphKnob->depthMenuLabel = "MORPH CV depth";
		addParam(morphKnob);

			auto* densKnob = createParamCentered<CvDepthKnob>(mm2px(Vec(24.0f, 63.0f)), module, NoiseVCO::DENS_PARAM);
			densKnob->moduleRef = module;
			densKnob->depthParam = NoiseVCO::DENS_CV_DEPTH_PARAM;
			densKnob->cvInput = NoiseVCO::DENS_CV_INPUT;
			densKnob->depthMenuLabel = "SCAN CV depth";
			addParam(densKnob);

			auto* smothKnob = createParamCentered<CvDepthKnob>(mm2px(Vec(38.0f, 63.0f)), module, NoiseVCO::SMOTH_PARAM);
			smothKnob->moduleRef = module;
			smothKnob->depthParam = NoiseVCO::SMOTH_CV_DEPTH_PARAM;
			smothKnob->cvInput = NoiseVCO::SMOTH_CV_INPUT;
			smothKnob->depthMenuLabel = "SPAN CV depth";
			addParam(smothKnob);

			auto* sizeKnob = createParamCentered<CvDepthKnob>(mm2px(Vec(52.0f, 63.0f)), module, NoiseVCO::WT_SIZE_PARAM);
			sizeKnob->moduleRef = module;
			sizeKnob->depthParam = NoiseVCO::WT_SIZE_CV_DEPTH_PARAM;
			sizeKnob->cvInput = NoiseVCO::WT_SIZE_CV_INPUT;
			sizeKnob->depthMenuLabel = "WT SIZE CV depth";
			addParam(sizeKnob);

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(52.0f, 20.0f)), module, NoiseVCO::GEN_TRIG_INPUT));
		addParam(createParamCentered<LEDButton>(mm2px(Vec(52.0f, 28.0f)), module, NoiseVCO::GEN_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(52.0f, 34.0f)), module, NoiseVCO::GEN_LIGHT));

					addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(10.0f, 79.0f)), module, NoiseVCO::ENV_PARAM));
				addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(24.0f, 79.0f)), module, NoiseVCO::RVB_TIME_PARAM));
				addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(38.0f, 79.0f)), module, NoiseVCO::RVB_FB_PARAM));
				addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(52.0f, 79.0f)), module, NoiseVCO::RVB_MIX_PARAM));

			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.0f, 95.0f)), module, NoiseVCO::MORPH_CV_INPUT));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(24.0f, 95.0f)), module, NoiseVCO::DENS_CV_INPUT));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(38.0f, 95.0f)), module, NoiseVCO::SMOTH_CV_INPUT));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(52.0f, 95.0f)), module, NoiseVCO::WT_SIZE_CV_INPUT));

			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.0f, 111.0f)), module, NoiseVCO::VOCT_INPUT));
				addInput(createInputCentered<PJ301MPort>(mm2px(Vec(24.0f, 111.0f)), module, NoiseVCO::TRIG_INPUT));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(38.0f, 111.0f)), module, NoiseVCO::LEFT_OUTPUT));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(52.0f, 111.0f)), module, NoiseVCO::RIGHT_OUTPUT));

		auto addPanelLabel = [this](float xMm, float yMm, const char* txt, int size = 8, NVGcolor color = nvgRGB(0x0f, 0x17, 0x2a)) {
			auto* l = createWidget<PanelLabel>(mm2px(Vec(xMm, yMm)));
			l->text = txt;
			l->fontSize = size;
			l->color = color;
			addChild(l);
		};

			addPanelLabel(30.5f, 7.5f, "WAVE FILE VCO", 10, nvgRGB(0x0b, 0x12, 0x20));
				addPanelLabel(10.0f, 41.0f, "PITCH", 7, nvgRGB(0x1f, 0x29, 0x37));
				addPanelLabel(24.0f, 41.0f, "DETUNE", 7, nvgRGB(0x1f, 0x29, 0x37));
				addPanelLabel(38.0f, 41.0f, "UNISON", 7, nvgRGB(0x1f, 0x29, 0x37));
				addPanelLabel(52.0f, 41.0f, "OCT", 7, nvgRGB(0x1f, 0x29, 0x37));

					addPanelLabel(10.0f, 57.0f, "MORPH", 8);
					addPanelLabel(24.0f, 57.0f, "SCAN", 8);
					addPanelLabel(38.0f, 57.0f, "SPAN", 8);
					addPanelLabel(52.0f, 57.0f, "WTSIZE", 8);
			addPanelLabel(52.0f, 14.0f, "JUMP TRIG", 7);
			addPanelLabel(52.0f, 22.0f, "JUMP", 8);
						addPanelLabel(10.0f, 73.0f, "ENV", 8);
					addPanelLabel(24.0f, 73.0f, "RVB TM", 8);
					addPanelLabel(38.0f, 73.0f, "RVB FB", 8);
					addPanelLabel(52.0f, 73.0f, "RVB MIX", 8);

			addPanelLabel(10.0f, 89.0f, "MRPH CV", 7);
			addPanelLabel(24.0f, 89.0f, "SCAN CV", 7);
			addPanelLabel(38.0f, 89.0f, "SPAN CV", 7);
			addPanelLabel(52.0f, 89.0f, "WT CV", 7);

			addPanelLabel(10.0f, 105.0f, "VOCT", 7);
			addPanelLabel(24.0f, 105.0f, "TRIG", 7);
			addPanelLabel(38.0f, 105.0f, "L OUT", 7);
			addPanelLabel(52.0f, 105.0f, "R OUT", 7);
		}

	void appendContextMenu(Menu* menu) override {
		ModuleWidget::appendContextMenu(menu);
		auto* m = dynamic_cast<NoiseVCO*>(module);
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

Model* modelWaveFileVCO = createModel<NoiseVCO, NoiseVCOWidget>("WaveFileVCO");
