#include "wavetable_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

constexpr int kDensMinTruePoints = 8;

float smoothstep01(float x) {
	float t = clamp(x, 0.f, 1.f);
	return t * t * (3.f - 2.f * t);
}

int densToTruePoints(int windowFrames, float densNorm) {
	int maxPoints = std::max(windowFrames, kDensMinTruePoints);
	float d = clamp(densNorm, 0.f, 1.f);
	if (maxPoints <= kDensMinTruePoints) {
		return kDensMinTruePoints;
	}

	// Exponential mapping tuned for 512-point playback table:
	// d=0.0 -> 8, d=0.75 -> ~48, d=1.0 -> maxPoints (e.g. 512).
	const float ratio = static_cast<float>(maxPoints) / static_cast<float>(kDensMinTruePoints);
	const float safeRatio = std::max(ratio, 1.f);
	const float targetAtThreeQuarter = clamp(
		static_cast<float>(kDensMinTruePoints) * 6.f,
		static_cast<float>(kDensMinTruePoints),
		static_cast<float>(maxPoints)
	);
	const float expAtThreeQuarter = std::log(targetAtThreeQuarter / static_cast<float>(kDensMinTruePoints)) /
	                                std::log(safeRatio);
	const float expThreeQuarterClamped = clamp(expAtThreeQuarter, 0.f, 1.f);
	float expo = 0.f;
	if (d < 0.75f) {
		float u = d / 0.75f;
		expo = expThreeQuarterClamped * smoothstep01(u);
	}
	else {
		float u = (d - 0.75f) / 0.25f;
		expo = expThreeQuarterClamped + (1.f - expThreeQuarterClamped) * smoothstep01(u);
	}

	float pointsF = static_cast<float>(kDensMinTruePoints) * std::pow(safeRatio, expo);
	int points = static_cast<int>(std::lround(pointsF));
	return clamp(points, kDensMinTruePoints, maxPoints);
}

int clampMode(int mode) {
	return clamp(mode, 0, WavetableEngine::kModeCount - 1);
}

void collectRisingZeroCrossings(const std::array<float, WavetableEngine::kGeneratedWavetableSize>& data,
                                int size,
                                std::vector<int>& zcOut) {
	zcOut.clear();
	for (int i = 1; i < size; ++i) {
		if (data[i - 1] <= 0.f && data[i] > 0.f) {
			zcOut.push_back(i);
		}
	}
}

float segmentSampleLinear(const std::array<float, WavetableEngine::kGeneratedWavetableSize>& data,
                          int start,
                          int len,
                          float t01) {
	if (len <= 1) {
		return data[clamp(start, 0, WavetableEngine::kGeneratedWavetableSize - 1)];
	}
	float p = clamp(t01, 0.f, 1.f) * static_cast<float>(len - 1);
	int i0 = clamp(start + static_cast<int>(std::floor(p)), 0, WavetableEngine::kGeneratedWavetableSize - 1);
	int i1 = clamp(i0 + 1, 0, WavetableEngine::kGeneratedWavetableSize - 1);
	float frac = p - std::floor(p);
	return data[i0] + (data[i1] - data[i0]) * frac;
}

float segmentCorrelationResampled(const std::array<float, WavetableEngine::kGeneratedWavetableSize>& data,
                                  int startA,
                                  int lenA,
                                  int startB,
                                  int lenB) {
	if (lenA < 8 || lenB < 8) {
		return -1.f;
	}
	const int n = clamp(std::max(lenA, lenB), 32, 256);
	float dot = 0.f;
	float ea = 0.f;
	float eb = 0.f;
	for (int i = 0; i < n; ++i) {
		float t = (n <= 1) ? 0.f : static_cast<float>(i) / static_cast<float>(n - 1);
		float a = segmentSampleLinear(data, startA, lenA, t);
		float b = segmentSampleLinear(data, startB, lenB, t);
		dot += a * b;
		ea += a * a;
		eb += b * b;
	}
	float denom = std::sqrt(std::max(ea * eb, 1e-12f));
	return dot / denom;
}

bool findBestZeroCrossNear(const std::vector<int>& zc,
                           int fromIndex,
                           int target,
                           int tol,
                           int& outIndex,
                           int& outPos) {
	outIndex = -1;
	outPos = -1;
	int bestDiff = 1e9;
	for (int i = std::max(fromIndex, 0); i < static_cast<int>(zc.size()); ++i) {
		int z = zc[i];
		if (z < target - tol) {
			continue;
		}
		if (z > target + tol) {
			break;
		}
		int diff = std::abs(z - target);
		if (diff < bestDiff) {
			bestDiff = diff;
			outIndex = i;
			outPos = z;
		}
	}
	return outIndex >= 0;
}

struct StableCycleCandidate {
	bool valid = false;
	int start = 0;
	int period = 0;
	int secondStart = 0;
	int secondPeriod = 0;
	float score = -1.f;
};

StableCycleCandidate detectStableCycleFromStart(const std::array<float, WavetableEngine::kGeneratedWavetableSize>& data,
                                                int size,
                                                bool pickBestScore) {
	std::vector<int> zc;
	collectRisingZeroCrossings(data, size, zc);
	if (zc.size() < 3) {
		return {};
	}
	StableCycleCandidate best;

	for (int iStart = 0; iStart + 2 < static_cast<int>(zc.size()); ++iStart) {
		int start = zc[iStart];
		for (int iNext = iStart + 1; iNext + 1 < static_cast<int>(zc.size()); ++iNext) {
			int b1 = zc[iNext];
			int period = b1 - start;
			if (period < 8 || period > size / 2) {
				continue;
			}

			int tol = std::max(2, period / 10);
			int iSecond = -1;
			int b2 = -1;
			if (!findBestZeroCrossNear(zc, iNext + 1, start + 2 * period, tol, iSecond, b2)) {
				continue;
			}
			int secondPeriod = b2 - b1;
			if (secondPeriod < 8) {
				continue;
			}

			float c12 = segmentCorrelationResampled(data, start, period, b1, secondPeriod);
			if (c12 < 0.55f) {
				continue;
			}

			float score = c12;
			int iThird = -1;
			int b3 = -1;
			if (findBestZeroCrossNear(zc, iSecond + 1, start + 3 * period, tol, iThird, b3)) {
				int thirdPeriod = b3 - b2;
				if (thirdPeriod >= 8) {
					float c23 = segmentCorrelationResampled(data, b1, secondPeriod, b2, thirdPeriod);
					if (c23 > 0.40f) {
						score = 0.5f * (c12 + c23);
					}
				}
			}

			if (score >= 0.62f) {
				StableCycleCandidate out;
				out.valid = true;
				out.start = start;
				out.period = period;
				out.secondStart = b1;
				out.secondPeriod = secondPeriod;
				out.score = score;
				if (!pickBestScore) {
					return out;
				}
				if (!best.valid || out.score > best.score) {
					best = out;
				}
			}
		}
	}
	return best;
}

int estimatePeriodAutocorrFromStart(const std::array<float, WavetableEngine::kGeneratedWavetableSize>& data, int size) {
	int minLag = 8;
	int maxLag = std::min(size / 2, 512);
	float bestScore = -1.f;
	int bestLag = -1;
	for (int lag = minLag; lag <= maxLag; ++lag) {
		int len = std::min(size - lag, 256);
		if (len < 32) {
			continue;
		}
		float dot = 0.f;
		float e0 = 0.f;
		float e1 = 0.f;
		for (int i = 0; i < len; ++i) {
			float a = data[i];
			float b = data[i + lag];
			dot += a * b;
			e0 += a * a;
			e1 += b * b;
		}
		float denom = std::sqrt(std::max(e0 * e1, 1e-12f));
		float score = dot / denom;
		if (score > bestScore) {
			bestScore = score;
			bestLag = lag;
		}
	}
	if (bestScore < 0.35f) {
		return -1;
	}
	return bestLag;
}

StableCycleCandidate detectAutocorrCycleFromStart(const std::array<float, WavetableEngine::kGeneratedWavetableSize>& data,
                                                  int size) {
	int period = estimatePeriodAutocorrFromStart(data, size);
	if (period < 8 || period >= size) {
		return {};
	}
	StableCycleCandidate out;
	out.valid = true;
	out.start = 0;
	out.period = period;
	if (period * 2 < size) {
		out.secondStart = period;
		out.secondPeriod = std::min(period, size - period);
	}
	return out;
}

StableCycleCandidate detectFirstZeroCrossCycleFromStart(const std::array<float, WavetableEngine::kGeneratedWavetableSize>& data,
                                                        int size) {
	std::vector<int> zc;
	collectRisingZeroCrossings(data, size, zc);
	for (int i = 0; i + 1 < static_cast<int>(zc.size()); ++i) {
		int start = zc[i];
		int period = zc[i + 1] - start;
		if (period < 8 || start + period > size) {
			continue;
		}
		StableCycleCandidate out;
		out.valid = true;
		out.start = start;
		out.period = period;
		if (i + 2 < static_cast<int>(zc.size())) {
			out.secondStart = zc[i + 1];
			out.secondPeriod = zc[i + 2] - zc[i + 1];
		}
		return out;
	}
	return {};
}

void fitCycleToWindow(std::array<float, WavetableEngine::kGeneratedWavetableSize>& out,
                      int outSize,
                      const std::vector<float>& cycle) {
	if (cycle.empty()) {
		return;
	}
	int cycleSize = static_cast<int>(cycle.size());
	if (cycleSize == 1) {
		for (int i = 0; i < outSize; ++i) {
			out[i] = cycle[0];
		}
		return;
	}
	const float outDen = static_cast<float>(std::max(1, outSize - 1));
	const float srcDen = static_cast<float>(cycleSize - 1);
	for (int i = 0; i < outSize; ++i) {
		float pos = (static_cast<float>(i) / outDen) * srcDen;
		int i0 = static_cast<int>(std::floor(pos));
		int i1 = std::min(i0 + 1, cycleSize - 1);
		float frac = pos - static_cast<float>(i0);
		float s0 = cycle[i0];
		float s1 = cycle[i1];
		out[i] = s0 + (s1 - s0) * frac;
	}
}

bool centerAndValidateCycle(std::vector<float>& cycle) {
	if (cycle.size() < 8) {
		return false;
	}
	float sum = 0.f;
	for (float v : cycle) {
		sum += v;
	}
	float mean = sum / static_cast<float>(cycle.size());

	float minV = 1e9f;
	float maxV = -1e9f;
	float maxAbs = 0.f;
	for (float& v : cycle) {
		v = clamp(v - mean, -1.f, 1.f);
		minV = std::min(minV, v);
		maxV = std::max(maxV, v);
		maxAbs = std::max(maxAbs, std::abs(v));
	}

	// Reject near-DC / near-flat cycle candidates which become trapezoids after taper.
	const float range = maxV - minV;
	return range > 0.04f && maxAbs > 0.02f;
}

} // namespace

WavetableEngine::WavetableEngine() {
}

WavetableEngine::~WavetableEngine() {
	stopWorkerThread();
}

const char* WavetableEngine::modeToShortLabel(int mode) {
	switch (clampMode(mode)) {
		case MODE_CYCLE_AC: return "C-AC";
		case MODE_CYCLE_ZC: return "C-ZC";
		case MODE_CYCLE_AVG2: return "C-AV2";
		default: return "FREE";
	}
}

void WavetableEngine::init(int wtSize, int dens, int smoth, float scanNorm, int mode) {
	buildTableState(
		wavetableStates[0],
		wtSize,
		dens,
		smoth,
		scanNorm,
		mode,
		std::shared_ptr<const std::vector<float>> {}
	);
	wavetableStates[1] = wavetableStates[0];
	wavetableStates[2] = wavetableStates[0];
	activeStateIndex.store(0, std::memory_order_relaxed);
	prevStateIndex.store(0, std::memory_order_relaxed);
	readyStateIndex.store(-1, std::memory_order_relaxed);
	requestedWtSize = -1;
	requestedDense = -1;
	requestedSmoth = -1;
	requestedScanNorm = -1.f;
	requestedMode = MODE_FREE;
	tableBlend.store(1.f, std::memory_order_relaxed);
	publishUiDisplayWave();
	setTargets(wtSize, dens, smoth, scanNorm, mode);
	startWorkerThread();
}

void WavetableEngine::forceRebuild(int wtSize, int dens, int smoth, float scanNorm, int mode) {
	int active = activeStateIndex.load(std::memory_order_acquire);
	prevStateIndex.store(active, std::memory_order_release);
	readyStateIndex.store(-1, std::memory_order_release);
	tableBlend.store(1.f, std::memory_order_relaxed);
	requestedWtSize = -1;
	requestedDense = -1;
	requestedSmoth = -1;
	requestedScanNorm = -1.f;
	requestedMode = MODE_FREE;
	setTargets(wtSize, dens, smoth, scanNorm, mode);
	publishUiDisplayWave();
}

void WavetableEngine::setSource(const std::shared_ptr<const std::vector<float>>& sourcePtr) {
	std::atomic_store_explicit(&sourceMonoActive, sourcePtr, std::memory_order_release);
	submitBuildRequest(
		(requestedWtSize >= 0) ? requestedWtSize : 512,
		(requestedDense >= 0) ? requestedDense : 100,
		(requestedSmoth >= 0) ? requestedSmoth : 0,
		(requestedScanNorm >= 0.f) ? requestedScanNorm : 0.f,
		requestedMode
	);
}

void WavetableEngine::setTargets(int wtSize, int dens, int smoth, float scanNorm, int mode) {
	int wt = clamp(wtSize, 256, kGeneratedWavetableSize);
	int de = clamp(dens, 0, 100);
	int sm = clamp(smoth, 0, 100);
	float sc = clamp(scanNorm, 0.f, 1.f);
	int mo = clampMode(mode);
	bool changed = (wt != requestedWtSize) ||
	               (de != requestedDense) ||
	               (sm != requestedSmoth) ||
	               (std::abs(sc - requestedScanNorm) > 1e-4f) ||
	               (mo != requestedMode);
	if (!changed) {
		return;
	}
	requestedWtSize = wt;
	requestedDense = de;
	requestedSmoth = sm;
	requestedScanNorm = sc;
	requestedMode = mo;
	submitBuildRequest(wt, de, sm, sc, mo);
}

void WavetableEngine::updateControl() {
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

void WavetableEngine::buildTableState(WavetableState& outState,
                                      int wtSizeParam,
                                      int dens,
                                      int smoth,
                                      float scanNorm,
                                      int mode,
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

	auto applyCycleFromDetection = [&](bool averageTwoCycles, bool pickBestScore, int fallbackMode) {
		StableCycleCandidate cand = detectStableCycleFromStart(localWindow, windowFrames, pickBestScore);
		if (!cand.valid) {
			if (fallbackMode == 1) {
				cand = detectFirstZeroCrossCycleFromStart(localWindow, windowFrames);
			}
			else if (fallbackMode == 2) {
				cand = detectAutocorrCycleFromStart(localWindow, windowFrames);
			}
		}
		if (!cand.valid || cand.period < 8 || cand.start + cand.period > windowFrames) {
			return false;
		}

		std::vector<float> cycle(cand.period);
		for (int i = 0; i < cand.period; ++i) {
			cycle[i] = localWindow[cand.start + i];
		}

		if (averageTwoCycles &&
		    cand.secondPeriod >= 8 &&
		    cand.secondStart + cand.secondPeriod <= windowFrames) {
			std::vector<float> avg(cand.period);
			const float den = static_cast<float>(std::max(1, cand.period - 1));
			for (int i = 0; i < cand.period; ++i) {
				float t = static_cast<float>(i) / den;
				float b = segmentSampleLinear(localWindow, cand.secondStart, cand.secondPeriod, t);
				avg[i] = 0.5f * (cycle[i] + b);
			}
			cycle.swap(avg);
		}

		if (!centerAndValidateCycle(cycle)) {
			return false;
		}
		fitCycleToWindow(localWindow, windowFrames, cycle);
		return true;
	};

	switch (clampMode(mode)) {
		case MODE_CYCLE_AC: {
			applyCycleFromDetection(false, true, 2);
			break;
		}
		case MODE_CYCLE_ZC: {
			applyCycleFromDetection(false, false, 1);
			break;
		}
		case MODE_CYCLE_AVG2: {
			if (!applyCycleFromDetection(true, true, 1)) {
				applyCycleFromDetection(false, true, 1);
			}
			break;
		}
		default:
			break;
	}

	int truePoints = densToTruePoints(windowFrames, densF);

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

	// Rounded trapezoid window:
	// 10% fade-in, 80% flat (1.0), 10% fade-out.
	const int edge = std::max(1, static_cast<int>(std::lround(0.10f * static_cast<float>(kGeneratedWavetableSize))));
	const int edgeClamped = std::min(edge, kGeneratedWavetableSize / 2);
	for (int i = 0; i < edgeClamped; ++i) {
		float t = static_cast<float>(i) / static_cast<float>(std::max(1, edgeClamped - 1));
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

int WavetableEngine::acquireBuildSlot() const {
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

void WavetableEngine::submitBuildRequest(int wtSize, int dens, int smoth, float scanNorm, int mode) {
	buildReqWtSize.store(clamp(wtSize, 256, kGeneratedWavetableSize), std::memory_order_relaxed);
	buildReqDense.store(clamp(dens, 0, 100), std::memory_order_relaxed);
	buildReqSmoth.store(clamp(smoth, 0, 100), std::memory_order_relaxed);
	buildReqScanNorm.store(clamp(scanNorm, 0.f, 1.f), std::memory_order_relaxed);
	buildReqMode.store(clampMode(mode), std::memory_order_relaxed);
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

			int slot = acquireBuildSlot();
			if (slot < 0) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}

			int wtSize = buildReqWtSize.load(std::memory_order_relaxed);
			int dens = buildReqDense.load(std::memory_order_relaxed);
			int smoth = buildReqSmoth.load(std::memory_order_relaxed);
			float scanNorm = buildReqScanNorm.load(std::memory_order_relaxed);
			int mode = buildReqMode.load(std::memory_order_relaxed);
			auto sourcePtr = std::atomic_load_explicit(&sourceMonoActive, std::memory_order_acquire);
			buildTableState(wavetableStates[slot], wtSize, dens, smoth, scanNorm, mode, sourcePtr);

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

void WavetableEngine::stopWorkerThread() {
	workerRunning.store(false, std::memory_order_release);
	if (workerThread.joinable()) {
		workerThread.join();
	}
}

void WavetableEngine::publishUiDisplayWave() {
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

void WavetableEngine::copyDisplayData(std::array<float, kMaxWavetableSize>& outData, int& outSize, float& outScan) const {
	int idx = uiDisplayWaveIndex.load(std::memory_order_acquire);
	outSize = kGeneratedWavetableSize;
	for (int i = 0; i < outSize; ++i) {
		outData[i] = uiDisplayWave[idx][i];
	}
	outScan = uiScanNorm.load(std::memory_order_relaxed);
}

int WavetableEngine::getPublishedWtSize() const {
	return uiWtSize.load(std::memory_order_relaxed);
}

float WavetableEngine::getPublishedScanNorm() const {
	return uiScanNorm.load(std::memory_order_relaxed);
}
