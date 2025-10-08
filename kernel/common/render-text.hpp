#pragma once

#include <cstddef>
#include <stdint.h>
#include <utility>

extern uint8_t fontBitmap[];

struct TextRenderer {
	TextRenderer(
	    volatile uint32_t *fbPtr,
	    size_t pitch,
	    uint8_t redMask,
	    uint8_t redShift,
	    uint8_t greenMask,
	    uint8_t greenShift,
	    uint8_t blueMask,
	    uint8_t blueShift
	)
	: _fbPtr{fbPtr},
	  _pitch{pitch},
	  _redMask{redMask},
	  _redShift{redShift},
	  _greenMask{greenMask},
	  _greenShift{greenShift},
	  _blueMask{blueMask},
	  _blueShift{blueShift} {
		_rgbColors[0] = rgb(1, 1, 1);
		_rgbColors[1] = rgb(222, 56, 43);
		_rgbColors[2] = rgb(57, 181, 74);
		_rgbColors[3] = rgb(255, 199, 6);
		_rgbColors[4] = rgb(0, 111, 184);
		_rgbColors[5] = rgb(118, 38, 113);
		_rgbColors[6] = rgb(44, 181, 233);
		_rgbColors[7] = rgb(204, 204, 204);
		_rgbColors[8] = rgb(128, 128, 128);
		_rgbColors[9] = rgb(255, 0, 0);
		_rgbColors[10] = rgb(0, 255, 0);
		_rgbColors[11] = rgb(255, 255, 0);
		_rgbColors[12] = rgb(0, 0, 255);
		_rgbColors[13] = rgb(255, 0, 255);
		_rgbColors[14] = rgb(0, 255, 255);
		_rgbColors[15] = rgb(255, 255, 255);

		_defaultBg = rgb(16, 16, 16);
	}

	template <int FontWidth, int FontHeight>
	void renderChars( //
        size_t x, size_t y, size_t fontScale, const char *c, size_t count, int fgColor, int bgColor,
        std::integral_constant<int, FontWidth>, std::integral_constant<int, FontHeight>
    ) const {
		auto fg = _rgbColors[fgColor];
		auto bg = bgColor < 0 ? _defaultBg : _rgbColors[bgColor];

		auto line = _fbPtr + y * (FontHeight * fontScale) * _pitch + x * (FontWidth * fontScale);
		for (size_t i = 0; i < FontHeight; i++) {
			auto dest = line;
			for (size_t k = 0; k < count; k++) {
				auto dc = (c[k] >= 32 && c[k] <= 127) ? c[k] : 127;
				auto bits = fontBitmap[(dc - 32) * FontHeight + i];
				for (size_t j = 0; j < FontWidth; j++) {
					auto bit = (1 << ((FontWidth - 1) - j));
					auto color = (bits & bit) ? fg : bg;
					auto block_start = dest;
					for (size_t sy = 0; sy < fontScale; sy++) {
						for (size_t sx = 0; sx < fontScale; sx++) {
							*dest++ = color;
						}
						dest += _pitch - fontScale;
					}
					dest = block_start + fontScale;
				}
			}
			line += _pitch * fontScale;
		}
	}

	size_t pitch() const { return _pitch; }

	uint8_t redMask() const { return _redMask; }
	uint8_t redShift() const { return _redShift; }
	uint8_t greenMask() const { return _greenMask; }
	uint8_t greenShift() const { return _greenShift; }
	uint8_t blueMask() const { return _blueMask; }
	uint8_t blueShift() const { return _blueShift; }

	uint32_t rgbColor(int index) const { return _rgbColors[index]; }
	uint32_t defaultBgColor() const { return _defaultBg; }

private:
	inline uint32_t rgb(unsigned int r, unsigned int g, unsigned int b) const {
		static const auto map = [](unsigned int v, unsigned int max) -> unsigned int {
			return (v * max) / 255;
		};

		r = map(r, (1 << _redMask) - 1);
		g = map(g, (1 << _greenMask) - 1);
		b = map(b, (1 << _blueMask) - 1);

		return (r << _redShift) | (g << _greenShift) | (b << _blueShift);
	}

	volatile uint32_t *_fbPtr;

	size_t _pitch;

	uint8_t _redMask;
	uint8_t _redShift;
	uint8_t _greenMask;
	uint8_t _greenShift;
	uint8_t _blueMask;
	uint8_t _blueShift;

	uint32_t _rgbColors[16];
	uint32_t _defaultBg;
};

inline int getFramebufferTextScale(int width, int height) {
	// Logic copied from
	// https://codeberg.org/Mintsuki/Flanterm/src/commit/55d228ff16234513b0df0dd12de8bc58160fc196/src/flanterm_backends/fb.c#L936-L947
	if (width >= (3840 + 3840 / 3) && height >= (2160 + 2160 / 3)) {
		return 4;
	} else if (width >= (1920 + 1920 / 3) && height >= (1080 + 1080 / 3)) {
		return 2;
	} else {
		return 1;
	}
}
