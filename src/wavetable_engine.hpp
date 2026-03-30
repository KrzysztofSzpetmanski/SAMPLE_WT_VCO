#pragma once

#include "plugin.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <random>
#include <thread>
#include <vector>

class WavetableEngine {
public:
	static constexpr int kMaxWavetableSize = 4096;
	static constexpr int kGeneratedWavetableSize = 512;
	static constexpr int kMorphWaveCount = 20;
	static constexpr int kMipLevels = 5;

	WavetableEngine();
	~WavetableEngine();

	void init(int wtSize, float scanNorm);
	void forceRebuild(int wtSize, float scanNorm);
	void setSource(const std::shared_ptr<const std::vector<float>>& sourcePtr);
	void setTargets(int wtSize, float scanNorm);
	void setMorphNorm(float morphNorm);
	void updateControl();
	void advanceBlend(float sampleTime, float transitionTimeSec);

	float readSample(float ph, float freq, float sampleRate) const;
	void copyDisplayWaves(std::array<std::array<float, kGeneratedWavetableSize>, kMorphWaveCount>& outWaves,
	                      int& outWaveCount,
	                      int& outWaveSize,
	                      float& outScan,
	                      float& outMorph) const;
	int getPublishedWtSize() const;
	float getPublishedScanNorm() const;
	float getPublishedMorphNorm() const;

private:
	struct BankSnapshot {
		std::array<std::array<float, kGeneratedWavetableSize>, kMorphWaveCount> wipWaves {};
		std::array<std::array<float, kGeneratedWavetableSize>, kMorphWaveCount> audioWaves {};
		std::array<std::array<std::array<float, kGeneratedWavetableSize>, kMipLevels>, kMorphWaveCount> mip {};
		std::array<std::array<int, kMipLevels>, kMorphWaveCount> mipSize {};
		int wtSize = 512;
		float scanNorm = 0.f;
		float scanFrac = 0.f;
		int leftStart = 0;
		int rightStart = 0;
		uint64_t sourceRevision = 0;
		bool valid = false;
	};

	float sanitizeWaveSample(float v) const;
	float readWavetableLevelSample(const std::array<std::array<float, kGeneratedWavetableSize>, kMipLevels>& mip,
	                               const std::array<int, kMipLevels>& mipSizes,
	                               int level,
	                               float ph) const;
	void selectMipLevels(float freq, float sampleRate, int& level0, int& level1, float& blend) const;
	void rebuildMipmapsFromTable(const std::array<float, kGeneratedWavetableSize>& source,
	                             std::array<std::array<float, kGeneratedWavetableSize>, kMipLevels>& mipOut,
	                             std::array<int, kMipLevels>& mipSizesOut);
	void buildSnapshot(BankSnapshot& out,
	                   int wtSize,
	                   float scanNorm,
	                   const std::shared_ptr<const std::vector<float>>& sourcePtr,
	                   uint64_t sourceRev);
	void submitBuildRequest(int wtSize, float scanNorm, uint64_t sourceRev);
	void startWorkerThread();
	void stopWorkerThread();

	mutable std::mt19937 rng {0x4e565f43u};

	std::shared_ptr<const std::vector<float>> sourceMonoActive;
	std::atomic<uint64_t> sourceRevision {1};

	std::shared_ptr<BankSnapshot> activeSnapshot;
	std::shared_ptr<BankSnapshot> prevSnapshot;
	std::shared_ptr<BankSnapshot> readySnapshot;

	int requestedWtSize = -1;
	float requestedScanNorm = -1.f;
	int requestedScanStartSample = -1;

	std::atomic<int> buildReqWtSize {512};
	std::atomic<float> buildReqScanNorm {0.f};
	std::atomic<uint64_t> buildReqSourceRevision {0};
	std::atomic<uint64_t> buildReqRevision {0};
	std::atomic<bool> workerRunning {false};
	std::thread workerThread;

	std::atomic<float> morphNorm {0.f};
	std::atomic<float> tableBlend {1.f};
	std::atomic<int> uiWtSize {512};
	std::atomic<float> uiScanNorm {0.f};
	std::atomic<float> uiMorphNorm {0.f};
};
