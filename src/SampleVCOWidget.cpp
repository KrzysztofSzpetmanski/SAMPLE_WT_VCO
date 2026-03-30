#include "SampleVCO.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <string>

#include <osdialog.h>

static constexpr std::array<int, 14> kDepthMenuSteps = {
	0, 5, 10, 15, 20, 25, 30, 40, 50, 60, 70, 80, 90, 100
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
		auto font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
		if (!moduleRef->hasLoadedSource()) {
			if (font) {
				nvgFontFaceId(args.vg, font->handle);
				nvgFontSize(args.vg, 10.f);
				nvgFillColor(args.vg, nvgRGB(0x1f, 0x29, 0x37));
				nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.5f, "NO FILE LOADED", nullptr);
			}
			return;
		}

		std::array<std::array<float, SampleVCO::kGeneratedWavetableSize>, SampleVCO::kMorphWaveCount> waves {};
		int waveCount = 0;
		int waveSize = 0;
		float scanNorm = 0.f;
		float morphNorm = 0.f;
		moduleRef->copyDisplayWaves(waves, waveCount, waveSize, scanNorm, morphNorm);
		if (waveCount < 1 || waveSize < 2) {
			return;
		}

		const float pad = 4.f;
		const float drawW = box.size.x - 2.f * pad;
		const float drawH = box.size.y - 2.f * pad;
		const float skewPct = 0.35f;
		const float depthPct = 0.62f;
		const float hCompress = 0.60f;

		const int step = 2;
		for (int w = waveCount - 1; w >= 0; w -= step) {
			float tWave = (waveCount <= 1) ? 0.f :
				static_cast<float>(w) / static_cast<float>(waveCount - 1);
			float x0 = pad + tWave * skewPct * drawW;
			float y0 = pad + (1.f - tWave) * depthPct * drawH;
			float lw = drawW * (1.f - skewPct);
			float hw = drawH * depthPct * hCompress;

			nvgBeginPath(args.vg);
			for (int i = 0; i < waveSize; ++i) {
				float t = static_cast<float>(i) / static_cast<float>(waveSize - 1);
				float x = x0 + t * lw;
				float y = y0 + (-waves[w][i] + 1.f) * 0.5f * hw;
				if (i == 0) {
					nvgMoveTo(args.vg, x, y);
				}
				else {
					nvgLineTo(args.vg, x, y);
				}
			}
			NVGcolor c = nvgRGB(0x0f, 0x76, 0xbc);
			c.a = 0.20f + 0.45f * (1.f - tWave);
			nvgStrokeWidth(args.vg, 1.0f);
			nvgStrokeColor(args.vg, c);
			nvgStroke(args.vg);
		}

		float selPos = clamp(morphNorm, 0.f, 1.f) * static_cast<float>(std::max(waveCount - 1, 0));
		int w0 = clamp(static_cast<int>(std::floor(selPos)), 0, std::max(waveCount - 1, 0));
		int w1 = std::min(w0 + 1, std::max(waveCount - 1, 0));
		float wf = selPos - static_cast<float>(w0);
		float tSel = (waveCount <= 1) ? 0.f : selPos / static_cast<float>(waveCount - 1);
		float sx0 = pad + tSel * skewPct * drawW;
		float sy0 = pad + (1.f - tSel) * depthPct * drawH;
		float slw = drawW * (1.f - skewPct);
		float shw = drawH * depthPct * hCompress;

		nvgBeginPath(args.vg);
		for (int i = 0; i < waveSize; ++i) {
			float t = static_cast<float>(i) / static_cast<float>(waveSize - 1);
			float x = sx0 + t * slw;
			float yWave = waves[w0][i] + (waves[w1][i] - waves[w0][i]) * wf;
			float y = sy0 + (-yWave + 1.f) * 0.5f * shw;
			if (i == 0) {
				nvgMoveTo(args.vg, x, y);
			}
			else {
				nvgLineTo(args.vg, x, y);
			}
		}
		nvgStrokeWidth(args.vg, 2.0f);
		nvgStrokeColor(args.vg, nvgRGB(0xef, 0x44, 0x44));
		nvgStroke(args.vg);

		if (font) {
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, 9.f);
			nvgFillColor(args.vg, nvgRGB(0x1f, 0x29, 0x37));
			nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			std::string info = rack::string::f("WT %d  SC %.2f  MR %.2f",
			                                   moduleRef->getPublishedWtSize(),
			                                   scanNorm,
			                                   morphNorm);
			nvgText(args.vg, 5.f, 4.f, info.c_str(), nullptr);

			std::string fileName = moduleRef->getSourceStatusString();
			const size_t maxChars = 27;
			if (fileName.size() > maxChars) {
				fileName = fileName.substr(0, maxChars - 3) + "...";
			}
			nvgFontSize(args.vg, 8.f);
			nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
			nvgText(args.vg, box.size.x * 0.5f, box.size.y - 2.f, fileName.c_str(), nullptr);
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
		wtDisplay->box.size = mm2px(Vec(41.5f, 22.0f));
		wtDisplay->moduleRef = module;
		addChild(wtDisplay);

		auto* morphKnob = createParamCentered<CvDepthKnob>(mm2px(Vec(52.0f, 31.5f)), module, SampleVCO::MORPH_PARAM);
		morphKnob->moduleRef = module;
		morphKnob->depthParam = SampleVCO::MORPH_CV_DEPTH_PARAM;
		morphKnob->cvInput = SampleVCO::MORPH_CV_INPUT;
		morphKnob->depthMenuLabel = "MORPH CV depth";
		addParam(morphKnob);

		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(10.0f, 53.0f)), module, SampleVCO::PITCH_PARAM));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(24.0f, 53.0f)), module, SampleVCO::DETUNE_PARAM));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(38.0f, 53.0f)), module, SampleVCO::UNISON_PARAM));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(52.0f, 53.0f)), module, SampleVCO::OCTAVE_PARAM));

		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(10.0f, 69.0f)), module, SampleVCO::SCAN_PARAM));

		auto* sizeKnob = createParamCentered<CvDepthKnob>(mm2px(Vec(24.0f, 69.0f)), module, SampleVCO::WT_SIZE_PARAM);
		sizeKnob->moduleRef = module;
		sizeKnob->depthParam = SampleVCO::WT_SIZE_CV_DEPTH_PARAM;
		sizeKnob->cvInput = SampleVCO::WT_SIZE_CV_INPUT;
		sizeKnob->depthMenuLabel = "WT SIZE CV depth";
		addParam(sizeKnob);

		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(10.0f, 85.0f)), module, SampleVCO::ENV_PARAM));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(24.0f, 85.0f)), module, SampleVCO::RVB_TIME_PARAM));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(38.0f, 85.0f)), module, SampleVCO::RVB_FB_PARAM));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(52.0f, 85.0f)), module, SampleVCO::RVB_MIX_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.0f, 101.0f)), module, SampleVCO::WT_SIZE_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(24.0f, 101.0f)), module, SampleVCO::MORPH_CV_INPUT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.0f, 117.0f)), module, SampleVCO::VOCT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(24.0f, 117.0f)), module, SampleVCO::TRIG_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(38.0f, 117.0f)), module, SampleVCO::LEFT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(52.0f, 117.0f)), module, SampleVCO::RIGHT_OUTPUT));

		auto addPanelLabel = [this](float xMm, float yMm, const std::string& txt, int size = 8, NVGcolor color = nvgRGB(0x0f, 0x17, 0x2a)) {
			auto* l = createWidget<PanelLabel>(mm2px(Vec(xMm, yMm)));
			l->text = txt;
			l->fontSize = size;
			l->color = color;
			addChild(l);
		};

		addPanelLabel(30.5f, 8.0f, "SAMPLE VCO", 10, nvgRGB(0x0b, 0x12, 0x20));
		addPanelLabel(52.0f, 10.0f, rack::string::f("BUILD %d", SampleVCO::kBuildNumber), 7);
		addPanelLabel(52.0f, 24.0f, "MORPH", 8);

		addPanelLabel(10.0f, 47.0f, "PITCH", 7, nvgRGB(0x1f, 0x29, 0x37));
		addPanelLabel(24.0f, 47.0f, "DETUNE", 7, nvgRGB(0x1f, 0x29, 0x37));
		addPanelLabel(38.0f, 47.0f, "UNISON", 7, nvgRGB(0x1f, 0x29, 0x37));
		addPanelLabel(52.0f, 47.0f, "OCT", 7, nvgRGB(0x1f, 0x29, 0x37));

		addPanelLabel(10.0f, 63.0f, "SCAN", 8);
		addPanelLabel(24.0f, 63.0f, "WTSIZE", 8);
		addPanelLabel(10.0f, 79.0f, "ENV", 8);

		addPanelLabel(24.0f, 79.0f, "RVB TM", 8);
		addPanelLabel(38.0f, 79.0f, "RVB FB", 8);
		addPanelLabel(52.0f, 79.0f, "RVB MIX", 8);

		addPanelLabel(10.0f, 95.0f, "WT CV", 7);
		addPanelLabel(24.0f, 95.0f, "MORPH CV", 7);

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
		menu->addChild(createMenuLabel("WAV source"));
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
