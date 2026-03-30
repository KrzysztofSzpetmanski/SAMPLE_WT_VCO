#pragma once

#include "plugin.hpp"
#include "reverb_stage.hpp"
#include "wavetable_engine.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct SampleVCO : Module {
	static constexpr int kMaxWavetableSize = WavetableEngine::kMaxWavetableSize;
	static constexpr int kGeneratedWavetableSize = WavetableEngine::kGeneratedWavetableSize;
	static constexpr int kMorphWaveCount = WavetableEngine::kMorphWaveCount;
	static constexpr int kBuildNumber = 100;
	static constexpr float kTableTransitionTimeSec = 0.02f;
	static constexpr float kControlUpdateIntervalSec = 0.01f;
	static constexpr int kMaxVoices = 10;

	enum ParamIds {
		PITCH_PARAM,
		DETUNE_PARAM,
		UNISON_PARAM,
		OCTAVE_PARAM,
		SCAN_PARAM,
		WT_SIZE_PARAM,
		MORPH_PARAM,
		ENV_PARAM,
		RVB_TIME_PARAM,
		RVB_FB_PARAM,
		RVB_MIX_PARAM,

		MORPH_CV_DEPTH_PARAM,
		WT_SIZE_CV_DEPTH_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		VOCT_INPUT,
		TRIG_INPUT,
		MORPH_CV_INPUT,
		WT_SIZE_CV_INPUT,
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

	SampleVCO();

	bool loadSourceWavPath(const std::string& path);
	void clearSourceWav();
	std::string getSourceStatusString() const;
	bool hasLoadedSource() const;
	void copySourceOverviewData(std::array<float, kMaxWavetableSize>& outData,
	                           int& outSize,
	                           float& outWindowStartNorm,
	                           float& outWindowSpanNorm) const;
	void copyDisplayWaves(std::array<std::array<float, kGeneratedWavetableSize>, kMorphWaveCount>& outWaves,
	                      int& outWaveCount,
	                      int& outWaveSize,
	                      float& outScan,
	                      float& outMorph) const;
	int getPublishedWtSize() const;
	float getModulatedKnobValue(float baseValue, int cvInputId, int depthParamId, float minV, float maxV);

	void onReset() override;
	json_t* dataToJson() override;
	void dataFromJson(json_t* rootJ) override;
	void process(const ProcessArgs& args) override;

private:
	float computeScanParam();
	float computeMorphParam();
	int computeWavetableSize();
	float computeEnvParam();
	float processEnvEnvelope(float trigVoltage, bool trigPatched, float env, float sampleTime);
	void updateTablesIfNeeded();
	static float sanitizeAudioOut(float v);

	dsp::SchmittTrigger contourTrigger;
	std::array<float, kMaxVoices> phase {};
	float controlUpdateTimer = 0.f;
	float contourEnvelope = 1.f;
	float previousSampleRate = 0.f;
	float scanSmoothed = 0.f;
	bool scanSmootherInit = false;

	WavetableEngine wavetableEngine;
	reverb_stage::ReverbStage reverbStage;

	std::shared_ptr<const std::vector<float>> sourceMonoPending;
	std::shared_ptr<const std::vector<float>> sourceMonoUi;
	std::atomic<bool> sourcePendingDirty {false};
	std::atomic<float> sourceSampleRate {0.f};
	std::atomic<bool> sourceLoaded {false};
	std::string sourcePath;
	std::string sourceError;
	mutable std::mutex sourceMetaMutex;
};

extern Model* modelWaveFileVCO;
