#include "SampleVCO.hpp"

#include "sample_loader.hpp"

#include <array>
#include <cmath>
#include <cstdlib>

struct PitchLikeSurgeQuantity : rack::engine::ParamQuantity {
	void setDisplayValueString(std::string s) override {
		float f = std::atof(s.c_str());
		if (f > 0.f) {
			float midi = 12.f * std::log2(f / 440.f) + 69.f;
			setValue((midi - 60.f) / 12.f);
		}
		else {
			setValue(0.f);
		}
	}

	std::string getDisplayValueString() override {
		float note = getValue() * 12.f + 60.f;
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
			setValue(reverb_stage::reverbTimeKnobFromSeconds(seconds));
		}
	}

	std::string getDisplayValueString() override {
		return rack::string::f("%.2f s", reverb_stage::reverbTimeSecondsFromKnob(getValue()));
	}
};

SampleVCO::SampleVCO() {
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
	auto* wtSizeQ = configParam(WT_SIZE_PARAM, 256.f, 512.f, 512.f, "WT size");
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
	configButton(MODE_PUSH_PARAM, "Mode switch");

	wavetableEngine.init(
		computeWavetableSize(),
		computeDenseParam(),
		computeSmothParam(),
		clamp(computeScanParam(), 0.f, 1.f),
		workMode
	);
	reverbStage.reset(48000.f);
}

float SampleVCO::sanitizeAudioOut(float v) {
	if (!std::isfinite(v)) {
		return 0.f;
	}
	return clamp(v, -10.f, 10.f);
}

float SampleVCO::getModulatedKnobValue(float baseValue, int cvInputId, int depthParamId, float minV, float maxV) {
	float depth = clamp(params[depthParamId].getValue(), 0.f, 1.f);
	float v = baseValue;
	if (inputs[cvInputId].isConnected()) {
		float cvNorm = clamp(inputs[cvInputId].getVoltage() / 5.f, -1.f, 1.f);
		float halfRange = 0.5f * (maxV - minV) * depth;
		v += cvNorm * halfRange;
	}
	return clamp(v, minV, maxV);
}

float SampleVCO::computeScanParam() {
	return getModulatedKnobValue(params[MORPH_PARAM].getValue(), MORPH_CV_INPUT, MORPH_CV_DEPTH_PARAM, 0.f, 1.f);
}

int SampleVCO::computeWavetableSize() {
	float v = getModulatedKnobValue(params[WT_SIZE_PARAM].getValue(), WT_SIZE_CV_INPUT,
	                                WT_SIZE_CV_DEPTH_PARAM, 256.f, 512.f);
	return clamp(static_cast<int>(std::round(v)), 256, 512);
}

int SampleVCO::computeDenseParam() {
	float v = getModulatedKnobValue(params[DENS_PARAM].getValue(), DENS_CV_INPUT,
	                                DENS_CV_DEPTH_PARAM, 0.f, 100.f);
	return clamp(static_cast<int>(std::round(v)), 0, 100);
}

int SampleVCO::computeSmothParam() {
	float v = getModulatedKnobValue(params[SMOTH_PARAM].getValue(), SMOTH_CV_INPUT,
	                                SMOTH_CV_DEPTH_PARAM, 0.f, 100.f);
	return clamp(static_cast<int>(std::round(v)), 0, 100);
}

float SampleVCO::computeEnvParam() {
	return clamp(params[ENV_PARAM].getValue(), 0.f, 1.f);
}

float SampleVCO::processEnvEnvelope(float trigVoltage, bool trigPatched, float env, float sampleTime) {
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

bool SampleVCO::loadSourceWavPath(const std::string& path) {
	std::vector<float> mono;
	float sr = 0.f;
	std::string err;
	if (!sample_loader::loadWavMonoLimited5s(path, mono, sr, err)) {
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

void SampleVCO::clearSourceWav() {
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

std::string SampleVCO::getSourceStatusString() const {
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

void SampleVCO::copySourceOverviewData(std::array<float, kMaxWavetableSize>& outData,
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
	int windowFrames = clamp(wavetableEngine.getPublishedWtSize(), 256, kGeneratedWavetableSize);
	windowFrames = std::min(windowFrames, srcSize);
	int maxStart = std::max(0, srcSize - windowFrames);
	float scan = clamp(wavetableEngine.getPublishedScanNorm(), 0.f, 1.f);
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
		outData[i] = clamp(s0 + (s1 - s0) * frac, -1.f, 1.f);
	}
}

void SampleVCO::copyDisplayData(std::array<float, kMaxWavetableSize>& outData, int& outSize, float& outScan) const {
	wavetableEngine.copyDisplayData(outData, outSize, outScan);
}

int SampleVCO::getPublishedWtSize() const {
	return wavetableEngine.getPublishedWtSize();
}

const char* SampleVCO::getWorkModeLabel() const {
	return WavetableEngine::modeToShortLabel(workMode);
}

void SampleVCO::updateModeSwitch() {
	if (modeButtonTrigger.process(params[MODE_PUSH_PARAM].getValue())) {
		workMode = (workMode + 1) % WavetableEngine::kModeCount;
	}
}

void SampleVCO::onReset() {
	for (float& p : phase) {
		p = 0.f;
	}
	wavetableEngine.forceRebuild(
		computeWavetableSize(),
		computeDenseParam(),
		computeSmothParam(),
		clamp(computeScanParam(), 0.f, 1.f),
		workMode
	);
	controlUpdateTimer = 0.f;
	contourEnvelope = 1.f;
	float sr = previousSampleRate > 1.f ? previousSampleRate : 48000.f;
	reverbStage.reset(sr);
}

json_t* SampleVCO::dataToJson() {
	json_t* rootJ = json_object();
	{
		std::lock_guard<std::mutex> lock(sourceMetaMutex);
		if (!sourcePath.empty()) {
			json_object_set_new(rootJ, "sourcePath", json_string(sourcePath.c_str()));
		}
	}
	json_object_set_new(rootJ, "workMode", json_integer(workMode));
	return rootJ;
}

void SampleVCO::dataFromJson(json_t* rootJ) {
	json_t* workModeJ = json_object_get(rootJ, "workMode");
	if (json_is_integer(workModeJ)) {
		workMode = clamp(static_cast<int>(json_integer_value(workModeJ)), 0, WavetableEngine::kModeCount - 1);
	}
	json_t* sourcePathJ = json_object_get(rootJ, "sourcePath");
	if (json_is_string(sourcePathJ)) {
		loadSourceWavPath(json_string_value(sourcePathJ));
	}
	wavetableEngine.forceRebuild(
		computeWavetableSize(),
		computeDenseParam(),
		computeSmothParam(),
		clamp(computeScanParam(), 0.f, 1.f),
		workMode
	);
}

void SampleVCO::updateTablesIfNeeded() {
	updateModeSwitch();

	if (sourcePendingDirty.exchange(false, std::memory_order_relaxed)) {
		auto pending = std::atomic_load_explicit(&sourceMonoPending, std::memory_order_acquire);
		wavetableEngine.setSource(pending);
	}

	wavetableEngine.setTargets(
		computeWavetableSize(),
		computeDenseParam(),
		computeSmothParam(),
		clamp(computeScanParam(), 0.f, 1.f),
		workMode
	);
	wavetableEngine.updateControl();
}

void SampleVCO::process(const ProcessArgs& args) {
	if (std::abs(args.sampleRate - previousSampleRate) > 1.f) {
		previousSampleRate = args.sampleRate;
		reverbStage.reset(args.sampleRate);
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

		float s = wavetableEngine.readSample(phase[v], freq, args.sampleRate);
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

	reverbStage.process(
		outL,
		outR,
		params[RVB_MIX_PARAM].getValue(),
		params[RVB_TIME_PARAM].getValue(),
		params[RVB_FB_PARAM].getValue(),
		args.sampleRate
	);

	outL = sanitizeAudioOut(outL);
	outR = sanitizeAudioOut(outR);
	wavetableEngine.advanceBlend(args.sampleTime, kTableTransitionTimeSec);

	outputs[LEFT_OUTPUT].setChannels(1);
	outputs[RIGHT_OUTPUT].setChannels(1);
	outputs[LEFT_OUTPUT].setVoltage(outL);
	outputs[RIGHT_OUTPUT].setVoltage(outR);

	float morphMod = inputs[MORPH_CV_INPUT].isConnected() ? params[MORPH_CV_DEPTH_PARAM].getValue() : 0.f;
	float sizeMod = inputs[WT_SIZE_CV_INPUT].isConnected() ? params[WT_SIZE_CV_DEPTH_PARAM].getValue() : 0.f;
	lights[MORPH_MOD_LIGHT].setBrightnessSmooth(morphMod, args.sampleTime * 12.f);
	lights[WT_SIZE_MOD_LIGHT].setBrightnessSmooth(sizeMod, args.sampleTime * 12.f);
}
