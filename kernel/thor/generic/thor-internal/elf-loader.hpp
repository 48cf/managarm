#pragma once

#include <frg/string.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/hierarchy.hpp>

namespace thor {

// What loadModuleImage() learned about an image while loading it.
//
// Everything except entryIp exists to build the aux vector for a dynamically
// linked image. A statically linked one only sets entryIp and leaves the
// interpreter empty.
struct ImageInfo {
	ImageInfo()
	: entryIp(nullptr), phdrPtr(nullptr), phdrEntrySize(0), phdrCount(0),
			interpreter(*kernelAlloc) { }

	void *entryIp;
	void *phdrPtr;
	size_t phdrEntrySize;
	size_t phdrCount;
	frg::string<KernelAlloc> interpreter;
};

// Maps the PT_LOAD segments of an ELF image into the given address space.
//
// Nothing is relocated, so a base of zero loads the image at the addresses it
// was linked for and requires ET_EXEC. A non-zero base is only meaningful for
// the ET_DYN interpreter.
coroutine<ImageInfo> loadModuleImage(
	smarter::shared_ptr<Hierarchy> hierarchy,
	smarter::shared_ptr<AddressSpace, BindableHandle> space,
	VirtualAddr base,
	smarter::shared_ptr<MemoryView> image
);

} // namespace thor
