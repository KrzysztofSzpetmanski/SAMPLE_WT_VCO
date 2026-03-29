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
	static constexpr float kTableTransitionTimeSec = 0.3f;
	static constexpr float kControlUpdateIntervalSec = 0.002f;
	static constexpr int kMaxVoices = 10;

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
		MODE_PUSH_PARAM,
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

	SampleVCO();

	bool loadSourceWavPath(const std::string& path);
	void clearSourceWav();
	std::string getSourceStatusString() const;
	void copySourceOverviewData(std::array<float, kMaxWavetableSize>& outData,
	                           int& outSize,
	                           float& outWindowStartNorm,
	                           float& outWindowSpanNorm) const;
	void copyDisplayData(std::array<float, kMaxWavetableSize>& outData, int& outSize, float& outScan) const;
	int getPublishedWtSize() const;
	const char* getWorkModeLabel() const;
	float getModulatedKnobValue(float baseValue, int cvInputId, int depthParamId, float minV, float maxV);

	void onReset() override;
	json_t* dataToJson() override;
	void dataFromJson(json_t* rootJ) override;
	void process(const ProcessArgs& args) override;

private:
	float computeScanParam();
	int computeWavetableSize();
	int computeDenseParam();
	int computeSmothParam();
	float computeEnvParam();
	float processEnvEnvelope(float trigVoltage, bool trigPatched, float env, float sampleTime);
	void updateModeSwitch();
	void updateTablesIfNeeded();
	static float sanitizeAudioOut(float v);

	dsp::SchmittTrigger contourTrigger;
	dsp::SchmittTrigger modeButtonTrigger;
	std::array<float, kMaxVoices> phase {};
	float controlUpdateTimer = 0.f;
	float contourEnvelope = 1.f;
	float previousSampleRate = 0.f;
	int workMode = WavetableEngine::MODE_FREE;

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
