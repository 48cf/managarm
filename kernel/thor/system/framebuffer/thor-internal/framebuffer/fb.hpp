#pragma once

#include <thor-internal/address-space.hpp>

namespace thor {

struct FbInfo {
	uint64_t address;
	uint64_t pitch;
	uint64_t width;
	uint64_t height;
	uint64_t bpp;
	uint32_t type;

	uint8_t redMask;
	uint8_t redShift;
	uint8_t greenMask;
	uint8_t greenShift;
	uint8_t blueMask;
	uint8_t blueShift;

	smarter::shared_ptr<MemoryView> memory;
};

void initializeBootFb(uint64_t address, uint64_t pitch, uint64_t width,
		uint64_t height, uint64_t bpp, EirFramebufferType type, void *early_window);
void transitionBootFb();

} // namespace thor
