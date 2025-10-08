#include <assert.h>

#include <eir-internal/debug.hpp>
#include <eir-internal/framebuffer.hpp>
#include <eir-internal/generic.hpp>
#include <frg/manual_box.hpp>
#include <render-text.hpp>

namespace eir {

namespace {

constexpr int fontWidth = 8;
constexpr int fontHeight = 16;

frg::optional<EirFramebuffer> &accessGlobalFb() {
	static frg::eternal<frg::optional<EirFramebuffer>> singleton;
	return *singleton;
}

struct FbLogHandler : LogHandler {
	FbLogHandler(TextRenderer renderer, int fontScale)
	: textRenderer_{renderer},
	  fontScale_{fontScale} {}

	// Check whether eir can log to this framebuffer.
	static bool suitable(const EirFramebuffer &fb) {
		if (fb.fbBpp != 32)
			return false;
		if (fb.fbAddress + fb.fbHeight * fb.fbPitch > UINTPTR_MAX)
			return false;
		return true;
	}

	void emit(frg::string_view line) override {
		auto &fb = *accessGlobalFb();
		for (size_t i = 0; i < line.size(); ++i) {
			auto c = line[i];
			if (c == '\n') {
				outputX_ = 0;
				outputY_++;
			} else if (outputX_ >= fb.fbWidth / (fontWidth * fontScale_)) {
				outputX_ = 0;
				outputY_++;
			} else if (outputY_ >= fb.fbHeight / (fontHeight * fontScale_)) {
				// TODO: Scroll.
			} else {
				textRenderer_.renderChars(
				    outputX_,
				    outputY_,
				    fontScale_,
				    &c,
				    1,
				    15,
				    -1,
				    std::integral_constant<int, fontWidth>{},
				    std::integral_constant<int, fontHeight>{}
				);
				outputX_++;
			}
		}
		outputX_ = 0;
		outputY_++;
	}

private:
	TextRenderer textRenderer_;
	unsigned int outputX_{0};
	unsigned int outputY_{0};
	int fontScale_{1};
};

constinit frg::manual_box<FbLogHandler> fbLogHandler;

} // anonymous namespace

void initFramebuffer(const EirFramebuffer &fb) {
	auto &globalFb = accessGlobalFb();
	// Right now, we only support a single FB.
	// If we want to support multiple ones, we may also need multiple log handlers
	// (e.g., because some may be suitable for eir logging while others may not be).
	assert(!globalFb);
	globalFb = fb;

	if (FbLogHandler::suitable(fb)) {
		auto [redMask, redShift, greenMask, greenShift, blueMask, blueShift] =
		    getFramebufferComponents(fb.fbType);

		TextRenderer renderer{
		    reinterpret_cast<volatile uint32_t *>(fb.fbAddress),
		    fb.fbPitch / sizeof(uint32_t),
		    redMask,
		    redShift,
		    greenMask,
		    greenShift,
		    blueMask,
		    blueShift
		};

		fbLogHandler.initialize(renderer, getFramebufferTextScale(fb.fbWidth, fb.fbHeight));

		enableLogHandler(fbLogHandler.get());
	} else {
		infoLogger() << "eir: Framebuffer is not suitable for logging" << frg::endlog;
	}
}

const EirFramebuffer *getFramebuffer() {
	auto &globalFb = accessGlobalFb();
	if (!globalFb)
		return nullptr;
	return &(*globalFb);
}
} // namespace eir
