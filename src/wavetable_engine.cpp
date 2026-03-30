#include "wavetable_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

constexpr int kRawWindowCount = WavetableEngine::kMorphWaveCount;
constexpr int kRawSpanFramesAtMaxWt = kRawWindowCount * WavetableEngine::kGeneratedWavetableSize;
constexpr int kScanBuildStride = 32;
constexpr float kBlendSwapReadyThreshold = 0.98f;

} // namespace

WavetableEngine::WavetableEngine() {
}

WavetableEngine::~WavetableEngine() {
	stopWorkerThread();
}

void WavetableEngine::init(int wtSize, float scanNorm) {
	auto initSnapshot = std::make_shared<BankSnapshot>();
	buildSnapshot(
		*initSnapshot,
		wtSize,
		scanNorm,
		std::shared_ptr<const std::vector<float>> {},
		sourceRevision.load(std::memory_order_relaxed)
	);
	std::atomic_store_explicit(&activeSnapshot, initSnapshot, std::memory_order_release);
	std::atomic_store_explicit(&prevSnapshot, initSnapshot, std::memory_order_release);
	std::shared_ptr<BankSnapshot> none;
	std::atomic_store_explicit(&readySnapshot, none, std::memory_order_release);

	uiWtSize.store(initSnapshot->wtSize, std::memory_order_relaxed);
	uiScanNorm.store(initSnapshot->scanNorm, std::memory_order_relaxed);
	tableBlend.store(1.f, std::memory_order_relaxed);

	requestedWtSize = -1;
	requestedScanNorm = -1.f;
	requestedScanStartSample = -1;
	setTargets(wtSize, scanNorm);
	startWorkerThread();
}

void WavetableEngine::forceRebuild(int wtSize, float scanNorm) {
	requestedWtSize = -1;
	requestedScanNorm = -1.f;
	requestedScanStartSample = -1;
	tableBlend.store(1.f, std::memory_order_relaxed);
	setTargets(wtSize, scanNorm);
}

void WavetableEngine::setSource(const std::shared_ptr<const std::vector<float>>& sourcePtr) {
	std::atomic_store_explicit(&sourceMonoActive, sourcePtr, std::memory_order_release);
	auto rev = sourceRevision.fetch_add(1, std::memory_order_acq_rel) + 1;
	if (requestedWtSize < 0 || requestedScanNorm < 0.f) {
		return;
	}
	submitBuildRequest(requestedWtSize, requestedScanNorm, rev);
}

void WavetableEngine::setTargets(int wtSize, float scanNorm) {
	int wt = clamp(wtSize, kMinWtSize, kGeneratedWavetableSize);
	float sc = clamp(scanNorm, 0.f, 1.f);
	auto srcPtr = std::atomic_load_explicit(&sourceMonoActive, std::memory_order_acquire);
	bool sourceReady = (srcPtr && srcPtr->size() > 2);
	int srcSize = sourceReady ? static_cast<int>(srcPtr->size()) : 0;
	int rawSpanFrames = sourceReady ? std::min(kRawSpanFramesAtMaxWt, srcSize) : kRawSpanFramesAtMaxWt;
	int maxStart = sourceReady ? std::max(0, srcSize - rawSpanFrames) : 0;

	int startSample = static_cast<int>(std::lround(sc * static_cast<float>(maxStart)));
	startSample = clamp(startSample, 0, maxStart);
	int qStart = (startSample / kScanBuildStride) * kScanBuildStride;
	qStart = clamp(qStart, 0, maxStart);
	float qScan = (maxStart > 0) ? (static_cast<float>(qStart) / static_cast<float>(maxStart)) : 0.f;

	bool changed = (wt != requestedWtSize) || (qStart != requestedScanStartSample);
	uiScanNorm.store(qScan, std::memory_order_relaxed);
	uiWtSize.store(wt, std::memory_order_relaxed);
	if (!changed) {
		return;
	}
	if (tableBlend.load(std::memory_order_relaxed) < kBlendSwapReadyThreshold) {
		return;
	}
	requestedWtSize = wt;
	requestedScanStartSample = qStart;
	requestedScanNorm = qScan;
	auto rev = sourceRevision.load(std::memory_order_acquire);
	submitBuildRequest(wt, qScan, rev);
}

void WavetableEngine::setMorphNorm(float morph) {
	morphNorm.store(clamp(morph, 0.f, 1.f), std::memory_order_relaxed);
}

void WavetableEngine::updateControl() {
	if (tableBlend.load(std::memory_order_relaxed) < kBlendSwapReadyThreshold) {
		uiMorphNorm.store(clamp(morphNorm.load(std::memory_order_relaxed), 0.f, 1.f), std::memory_order_relaxed);
		return;
	}

	auto ready = std::atomic_exchange_explicit(&readySnapshot, std::shared_ptr<BankSnapshot> {}, std::memory_order_acq_rel);
	if (ready && ready->valid) {
		auto active = std::atomic_load_explicit(&activeSnapshot, std::memory_order_acquire);
		if (!active || !active->valid) {
			active = ready;
		}
		std::atomic_store_explicit(&prevSnapshot, active, std::memory_order_release);
		std::atomic_store_explicit(&activeSnapshot, ready, std::memory_order_release);
		tableBlend.store(0.f, std::memory_order_relaxed);
		uiWtSize.store(ready->wtSize, std::memory_order_relaxed);
		uiScanNorm.store(ready->scanNorm, std::memory_order_relaxed);
	}
	uiMorphNorm.store(clamp(morphNorm.load(std::memory_order_relaxed), 0.f, 1.f), std::memory_order_relaxed);
}

void WavetableEngine::advanceBlend(float sampleTime, float transitionTimeSec) {
	float blend = tableBlend.load(std::memory_order_relaxed);
	blend = clamp(blend + sampleTime / std::max(transitionTimeSec, 1e-4f), 0.f, 1.f);
	tableBlend.store(blend, std::memory_order_relaxed);
}

float WavetableEngine::sanitizeWaveSample(float v) const {
	if (!std::isfinite(v)) {
		return 0.f;
	}
	return clamp(v, -1.f, 1.f);
}

float WavetableEngine::readWavetableLevelSample(const std::array<std::array<float, kGeneratedWavetableSize>, kMipLevels>& mip,
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

void WavetableEngine::selectMipLevels(float freq, float sampleRate, int& level0, int& level1, float& blend) const {
	float sr = std::max(sampleRate, 1000.f);
	float f = std::max(std::abs(freq), 1.f);
	float desiredSize = clamp(sr / f, 128.f, static_cast<float>(kGeneratedWavetableSize));
	float rawLevel = std::log2(static_cast<float>(kGeneratedWavetableSize) / desiredSize);
	float levelF = clamp(rawLevel + 0.20f, 0.f, static_cast<float>(kMipLevels - 1));
	level0 = static_cast<int>(std::floor(levelF));
	level1 = std::min(level0 + 1, kMipLevels - 1);
	blend = levelF - static_cast<float>(level0);
}

float WavetableEngine::readSample(float ph, float freq, float sampleRate) const {
	int level0 = 0;
	int level1 = 0;
	float levelBlend = 0.f;
	selectMipLevels(freq, sampleRate, level0, level1, levelBlend);

	auto readMorphState = [&](const std::shared_ptr<BankSnapshot>& bank, float morph) {
		if (!bank || !bank->valid) {
			return 0.f;
		}
		float m = clamp(morph, 0.f, 1.f) * static_cast<float>(kMorphWaveCount - 1);
		int w0 = clamp(static_cast<int>(std::floor(m)), 0, kMorphWaveCount - 1);
		int w1 = std::min(w0 + 1, kMorphWaveCount - 1);
		float wf = m - static_cast<float>(w0);

		float a0 = readWavetableLevelSample(bank->mip[w0], bank->mipSize[w0], level0, ph);
		float a1 = readWavetableLevelSample(bank->mip[w0], bank->mipSize[w0], level1, ph);
		float wa = sanitizeWaveSample(a0 + (a1 - a0) * levelBlend);

		float b0 = readWavetableLevelSample(bank->mip[w1], bank->mipSize[w1], level0, ph);
		float b1 = readWavetableLevelSample(bank->mip[w1], bank->mipSize[w1], level1, ph);
		float wb = sanitizeWaveSample(b0 + (b1 - b0) * levelBlend);

		return sanitizeWaveSample(wa + (wb - wa) * wf);
	};

	float morph = morphNorm.load(std::memory_order_relaxed);
	auto active = std::atomic_load_explicit(&activeSnapshot, std::memory_order_acquire);
	auto prev = std::atomic_load_explicit(&prevSnapshot, std::memory_order_acquire);

	float curr = readMorphState(active, morph);
	float blend = clamp(tableBlend.load(std::memory_order_relaxed), 0.f, 1.f);
	if (blend >= 0.999f || !prev || prev == active) {
		return curr;
	}
	float old = readMorphState(prev, morph);
	return sanitizeWaveSample(old + (curr - old) * blend);
}

void WavetableEngine::rebuildMipmapsFromTable(const std::array<float, kGeneratedWavetableSize>& source,
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

void WavetableEngine::buildSnapshot(BankSnapshot& out,
                                    int wtSize,
                                    float scanNorm,
                                    const std::shared_ptr<const std::vector<float>>& sourcePtr,
                                    uint64_t sourceRev) {
	int windowFrames = clamp(wtSize, kMinWtSize, kGeneratedWavetableSize);
	bool sourceReady = (sourcePtr && sourcePtr->size() > 2);
	int srcSize = sourceReady ? static_cast<int>(sourcePtr->size()) : 0;
	if (sourceReady) {
		windowFrames = std::min(windowFrames, srcSize);
	}

	int rawSpanFrames = kRawSpanFramesAtMaxWt;
	if (sourceReady) {
		rawSpanFrames = std::min(rawSpanFrames, srcSize);
	}
	if (rawSpanFrames < 2) {
		rawSpanFrames = 2;
	}

	int maxStart = sourceReady ? std::max(0, srcSize - rawSpanFrames) : 0;
	float scan = clamp(scanNorm, 0.f, 1.f);
	float scanPos = scan * static_cast<float>(maxStart);
	scanPos = clamp(scanPos, 0.f, static_cast<float>(maxStart));
	int leftStart = clamp(static_cast<int>(std::floor(scanPos)), 0, maxStart);
	int rightStart = std::min(leftStart + 1, maxStart);
	float scanFrac = clamp(scanPos - static_cast<float>(leftStart), 0.f, 1.f);
	if (rightStart == leftStart) {
		scanFrac = 0.f;
	}

	std::array<float, kRawSpanFramesAtMaxWt> rawLeft;
	std::array<float, kRawSpanFramesAtMaxWt> rawRight;
	if (sourceReady) {
		for (int i = 0; i < rawSpanFrames; ++i) {
			rawLeft[i] = sanitizeWaveSample((*sourcePtr)[leftStart + i]);
			rawRight[i] = sanitizeWaveSample((*sourcePtr)[rightStart + i]);
		}
	}
	else {
		for (int i = 0; i < rawSpanFrames; ++i) {
			rawLeft[i] = 0.f;
			rawRight[i] = 0.f;
		}
	}

	auto readRaw = [&](const std::array<float, kRawSpanFramesAtMaxWt>& raw, int size, float pos) {
		float p = clamp(pos, 0.f, static_cast<float>(std::max(0, size - 1)));
		int i0 = clamp(static_cast<int>(std::floor(p)), 0, size - 1);
		int i1 = std::min(i0 + 1, size - 1);
		float frac = p - static_cast<float>(i0);
		return sanitizeWaveSample(raw[i0] + (raw[i1] - raw[i0]) * frac);
	};

	const float outDen = static_cast<float>(kGeneratedWavetableSize - 1);
	const float pi = 3.14159265359f;
	const int edge = std::max(1, static_cast<int>(std::lround(0.10f * static_cast<float>(kGeneratedWavetableSize))));
	const int edgeClamped = std::min(edge, kGeneratedWavetableSize / 2);

	for (int w = 0; w < kMorphWaveCount; ++w) {
		float windowStart = static_cast<float>(w * windowFrames);
		float maxWindowStart = static_cast<float>(std::max(0, rawSpanFrames - windowFrames));
		windowStart = clamp(windowStart, 0.f, maxWindowStart);

		for (int i = 0; i < kGeneratedWavetableSize; ++i) {
			float posInWindow = (static_cast<float>(i) / outDen) * static_cast<float>(windowFrames - 1);
			float rawPos = windowStart + posInWindow;
			float a = readRaw(rawLeft, rawSpanFrames, rawPos);
			float b = readRaw(rawRight, rawSpanFrames, rawPos);
			out.wipWaves[w][i] = sanitizeWaveSample(a + (b - a) * scanFrac);
			out.audioWaves[w][i] = out.wipWaves[w][i];
		}

		for (int i = 0; i < edgeClamped; ++i) {
			float t = static_cast<float>(i) / static_cast<float>(std::max(1, edgeClamped - 1));
			float ww = 0.5f - 0.5f * std::cos(pi * t);
			out.audioWaves[w][i] = sanitizeWaveSample(out.audioWaves[w][i] * ww);
			out.audioWaves[w][kGeneratedWavetableSize - 1 - i] =
				sanitizeWaveSample(out.audioWaves[w][kGeneratedWavetableSize - 1 - i] * ww);
		}
		out.audioWaves[w][0] = 0.f;
		out.audioWaves[w][kGeneratedWavetableSize - 1] = 0.f;
		rebuildMipmapsFromTable(out.audioWaves[w], out.mip[w], out.mipSize[w]);
	}

	out.wtSize = windowFrames;
	out.scanNorm = scan;
	out.scanFrac = scanFrac;
	out.leftStart = leftStart;
	out.rightStart = rightStart;
	out.sourceRevision = sourceRev;
	out.valid = true;
}

void WavetableEngine::submitBuildRequest(int wtSize, float scanNorm, uint64_t sourceRev) {
	buildReqWtSize.store(clamp(wtSize, kMinWtSize, kGeneratedWavetableSize), std::memory_order_relaxed);
	buildReqScanNorm.store(clamp(scanNorm, 0.f, 1.f), std::memory_order_relaxed);
	buildReqSourceRevision.store(sourceRev, std::memory_order_relaxed);
	buildReqRevision.fetch_add(1, std::memory_order_release);
}

void WavetableEngine::startWorkerThread() {
	if (workerRunning.exchange(true, std::memory_order_acq_rel)) {
		return;
	}
	workerThread = std::thread([this]() {
		uint64_t seenRevision = buildReqRevision.load(std::memory_order_acquire);
		while (workerRunning.load(std::memory_order_acquire)) {
			uint64_t revision = buildReqRevision.load(std::memory_order_acquire);
			if (revision == seenRevision) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}

			int wt = buildReqWtSize.load(std::memory_order_relaxed);
			float sc = buildReqScanNorm.load(std::memory_order_relaxed);
			uint64_t srcRev = buildReqSourceRevision.load(std::memory_order_relaxed);
			auto srcPtr = std::atomic_load_explicit(&sourceMonoActive, std::memory_order_acquire);

			auto built = std::make_shared<BankSnapshot>();
			buildSnapshot(*built, wt, sc, srcPtr, srcRev);

			uint64_t revisionAfter = buildReqRevision.load(std::memory_order_acquire);
			if (revisionAfter != revision) {
				seenRevision = revision;
				continue;
			}

			std::atomic_store_explicit(&readySnapshot, built, std::memory_order_release);
			seenRevision = revision;
		}
	});
}

void WavetableEngine::stopWorkerThread() {
	workerRunning.store(false, std::memory_order_release);
	if (workerThread.joinable()) {
		workerThread.join();
	}
}

void WavetableEngine::copyDisplayWaves(std::array<std::array<float, kGeneratedWavetableSize>, kMorphWaveCount>& outWaves,
                                       int& outWaveCount,
                                       int& outWaveSize,
                                       float& outScan,
                                       float& outMorph) const {
	auto active = std::atomic_load_explicit(&activeSnapshot, std::memory_order_acquire);
	auto prev = std::atomic_load_explicit(&prevSnapshot, std::memory_order_acquire);
	if (!active) {
		outWaveCount = 0;
		outWaveSize = 0;
		outScan = 0.f;
		outMorph = 0.f;
		return;
	}
	if (!prev) {
		prev = active;
	}

	float blend = clamp(tableBlend.load(std::memory_order_relaxed), 0.f, 1.f);
	outWaveCount = kMorphWaveCount;
	outWaveSize = kGeneratedWavetableSize;
	for (int w = 0; w < outWaveCount; ++w) {
		for (int i = 0; i < outWaveSize; ++i) {
			float a = prev->wipWaves[w][i];
			float b = active->wipWaves[w][i];
			outWaves[w][i] = sanitizeWaveSample(a + (b - a) * blend);
		}
	}
	outScan = uiScanNorm.load(std::memory_order_relaxed);
	outMorph = clamp(morphNorm.load(std::memory_order_relaxed), 0.f, 1.f);
}

int WavetableEngine::getPublishedWtSize() const {
	return uiWtSize.load(std::memory_order_relaxed);
}

float WavetableEngine::getPublishedScanNorm() const {
	return uiScanNorm.load(std::memory_order_relaxed);
}

float WavetableEngine::getPublishedMorphNorm() const {
	return uiMorphNorm.load(std::memory_order_relaxed);
}
