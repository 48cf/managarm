
#include <render-text.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/io.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/pci/pci.hpp>

#include <thor-internal/framebuffer/fb.hpp>
#include <thor-internal/framebuffer/boot-screen.hpp>

#include <hw.frigg_bragi.hpp>

namespace thor {

namespace {

inline uint32_t fourcc(char a, char b, char c, char d) {
	return static_cast<uint32_t>(a)
		| (static_cast<uint32_t>(b) << 8)
		| (static_cast<uint32_t>(c) << 16)
		| (static_cast<uint32_t>(d) << 24);
}

} // namespace

// ------------------------------------------------------------------------
// window handling
// ------------------------------------------------------------------------

constexpr size_t fontHeight = 16;
constexpr size_t fontWidth = 8;

struct FbDisplay final : TextDisplay {
	FbDisplay(void *ptr, unsigned int width, unsigned int height, size_t pitch,
			uint8_t redMask, uint8_t redShift, uint8_t greenMask, uint8_t greenShift,
			uint8_t blueMask, uint8_t blueShift)
	: _width{width},
	  _height{height},
	  _pitch{pitch / sizeof(uint32_t)},
	  _textRenderer{
	      reinterpret_cast<volatile uint32_t *>(ptr),
	      pitch / sizeof(uint32_t),
	      redMask,
	      redShift,
	      greenMask,
	      greenShift,
	      blueMask,
	      blueShift
	  },
	  _fontScale{getFramebufferTextScale(width, height)} {
		assert(!(pitch % sizeof(uint32_t)));
		setWindow(ptr);
		_clearScreen(_textRenderer.defaultBgColor());
	}

	void setWindow(void *ptr) {
		_window = reinterpret_cast<uint32_t *>(ptr);
		_textRenderer = {
		    reinterpret_cast<volatile uint32_t *>(ptr),
		    _textRenderer.pitch(),
		    _textRenderer.redMask(),
		    _textRenderer.redShift(),
		    _textRenderer.greenMask(),
		    _textRenderer.greenShift(),
		    _textRenderer.blueMask(),
		    _textRenderer.blueShift()
		};
	}

	size_t getWidth() override;
	size_t getHeight() override;

	void setChars(unsigned int x, unsigned int y,
			const char *c, int count, int fg, int bg) override;
	void setBlanks(unsigned int x, unsigned int y, int count, int bg) override;

private:
	void _clearScreen(uint32_t rgb_color);

	volatile uint32_t *_window;
	unsigned int _width;
	unsigned int _height;
	size_t _pitch;
	TextRenderer _textRenderer;
	int _fontScale{1};
};

size_t FbDisplay::getWidth() {
	return _width / (fontWidth * _fontScale);
}

size_t FbDisplay::getHeight() {
	return _height / (fontHeight * _fontScale);
}

void FbDisplay::setChars(unsigned int x, unsigned int y,
		const char *c, int count, int fg, int bg) {
	_textRenderer.renderChars(x, y, _fontScale, c, count, fg, bg,
			std::integral_constant<int, fontWidth>{},
			std::integral_constant<int, fontHeight>{});
}

void FbDisplay::setBlanks(unsigned int x, unsigned int y, int count, int bg) {
	auto bg_rgb = (bg < 0) ? _textRenderer.defaultBgColor() : _textRenderer.rgbColor(bg);

	auto dest_line = _window + y * (fontHeight * _fontScale) * _pitch + x * (fontWidth * _fontScale);
	for(size_t i = 0; i < fontHeight * _fontScale; i++) {
		auto dest = dest_line;
		for(int k = 0; k < count; k++) {
			for(size_t j = 0; j < fontWidth * _fontScale; j++)
				*dest++ = bg_rgb;
		}
		dest_line += _pitch;
	}
}

void FbDisplay::_clearScreen(uint32_t rgb_color) {
	auto dest_line = _window;
	for(size_t i = 0; i < _height; i++) {
		auto dest = dest_line;
		for(size_t j = 0; j < _width; j++)
			*dest++ = rgb_color;
		dest_line += _pitch;
	}
}

namespace {
	frg::manual_box<FbInfo> bootInfo;
	frg::manual_box<FbDisplay> bootDisplay;
	frg::manual_box<BootScreen> bootScreen;
}

void initializeBootFb(uint64_t address, uint64_t pitch, uint64_t width,
		uint64_t height, uint64_t bpp, EirFramebufferType type, void *early_window) {
	bootInfo.initialize();
	auto fb_info = bootInfo.get();
	fb_info->address = address;
	fb_info->pitch = pitch;
	fb_info->width = width;
	fb_info->height = height;
	fb_info->bpp = bpp;

	std::tie(
	    fb_info->redMask,
	    fb_info->redShift,
	    fb_info->greenMask,
	    fb_info->greenShift,
	    fb_info->blueMask,
	    fb_info->blueShift
	) = getFramebufferComponents(type);

	switch (type) {
		case EirFramebufferType::x8r8g8b8:
			fb_info->type = fourcc('X', 'R', '2', '4');
			break;
		case EirFramebufferType::x8b8g8r8:
			fb_info->type = fourcc('X', 'B', '2', '4');
			break;
		case EirFramebufferType::x2r10g10b10:
			fb_info->type = fourcc('X', 'R', '3', '0');
			break;
	}

	// Initialize the framebuffer with a lower-half window.
	bootDisplay.initialize(early_window,
			fb_info->width, fb_info->height, fb_info->pitch,
			fb_info->redMask, fb_info->redShift,
			fb_info->greenMask, fb_info->greenShift,
			fb_info->blueMask, fb_info->blueShift);
	bootScreen.initialize(bootDisplay.get());

	enableLogHandler(bootScreen.get());
}

void transitionBootFb() {
	if(!bootInfo->address) {
		infoLogger() << "thor: No boot framebuffer" << frg::endlog;
		return;
	}

	auto windowSize = (bootInfo->height * bootInfo->pitch + (kPageSize - 1)) & ~(kPageSize - 1);
	auto window = KernelVirtualMemory::global().allocate(windowSize);
	for(size_t pg = 0; pg < windowSize; pg += kPageSize)
		KernelPageSpace::global().mapSingle4k(VirtualAddr(window) + pg,
				bootInfo->address + pg, page_access::write, CachingMode::writeCombine);

	// Transition to the kernel mapping window.
	bootDisplay->setWindow(window);

	assert(!(bootInfo->address & (kPageSize - 1)));
	bootInfo->memory = smarter::allocate_shared<HardwareMemory>(*kernelAlloc,
			bootInfo->address & ~(kPageSize - 1),
			(bootInfo->height * bootInfo->pitch + (kPageSize - 1)) & ~(kPageSize - 1),
			CachingMode::writeCombine);

	// Try to attached the framebuffer to a PCI device.
	pci::PciDevice *owner = nullptr;
	for (auto dev : *pci::allDevices) {
		auto checkBars = [&] () -> bool {
			for(int i = 0; i < 6; i++) {
				if(dev->bars[i].type != pci::PciBar::kBarMemory)
					continue;
				// TODO: Careful about overflow here.
				auto bar_begin = dev->bars[i].address;
				auto bar_end = dev->bars[i].address + dev->bars[i].length;
				if(bootInfo->address >= bar_begin
						&& bootInfo->address + bootInfo->height * bootInfo->pitch <= bar_end)
					return true;
			}

			return false;
		};

		if(checkBars()) {
			assert(!owner);
			owner = dev.get();
		}
	}

	if(!owner) {
		infoLogger() << "thor: Could not find owner for boot framebuffer" << frg::endlog;
		return;
	}

	infoLogger() << "thor: Boot framebuffer is attached to PCI device "
			<< owner->bus << "." << owner->slot << "." << owner->function << frg::endlog;
	owner->associatedFrameBuffer = bootInfo.get();
	owner->associatedScreen = bootScreen.get();
}

} // namespace thor

