#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <frg/manual_box.hpp>
#include <frg/slab.hpp>

#include <hel.h>

namespace {

constexpr size_t pageSize = 0x1000;

// Rounds up to a multiple of `alignment`, which must be a power of two.
constexpr size_t alignUp(size_t value, size_t alignment) {
	return (value + alignment - 1) & ~(alignment - 1);
}

// The hierarchy that heap memory is charged to. Set by helHeapInitialize()
// from the bootstrap data before the first allocation.
constinit HelHandle hierarchyHandle = kHelNullHandle;

struct HelVirtualAllocator {
	uintptr_t map(size_t length) {
		length = alignUp(length, pageSize);

		HelHandle memory;
		if (helAllocateMemory(hierarchyHandle, length, kHelAllocOnDemand,
				nullptr, &memory) != kHelErrNone)
			return 0;

		void *pointer;
		auto res = helMapMemory(memory, kHelNullHandle, nullptr, 0, length,
				kHelMapProtRead | kHelMapProtWrite, &pointer);

		// The mapping holds its own reference to the memory object.
		helCloseDescriptor(kHelThisUniverse, memory);

		if (res != kHelErrNone)
			return 0;
		return reinterpret_cast<uintptr_t>(pointer);
	}

	void unmap(uintptr_t address, size_t length) {
		helUnmapMemory(kHelNullHandle, reinterpret_cast<void *>(address),
				alignUp(length, pageSize));
	}
};

// Freya is single-threaded, so the pool needs no real mutex.
struct NullLock {
	void lock() {}
	void unlock() {}
};

using SlabPool = frg::slab_pool<HelVirtualAllocator, NullLock>;

constinit HelVirtualAllocator virtualAllocator;
constinit frg::manual_box<SlabPool> slabPool;

SlabPool &getSlabPool() {
	// We initialize the slab pool without a lock under the same assumption
	// that lets the pool run without a real mutex.
	if (!slabPool.valid())
		slabPool.initialize(virtualAllocator);
	return *slabPool;
}

} // anonymous namespace

extern "C" {

void frg_log(const char *str) {
	helLog(kHelLogSeverityWarning, str, strlen(str));
}

void frg_panic(const char *str) {
	helLog(kHelLogSeverityError, str, strlen(str));
	helPanic(str, strlen(str));
}

void helHeapInitialize(HelHandle hierarchy) {
	hierarchyHandle = hierarchy;
}

void *helHeapAllocate(size_t size, size_t align) {
	auto pointer = getSlabPool().allocate(alignUp(size, align));
	if (!pointer)
		return nullptr;

	// Aligning the allocation size to 2^a should yield a block from a bucket
	// that is aligned to 2^a, so let's verify this invariant before handing
	// a potentially misaligned pointer back to Rust.
	if (reinterpret_cast<uintptr_t>(pointer) & (align - 1))
		frg_panic("freya: slab pool returned an insufficiently aligned block");

	return pointer;
}

void helHeapDeallocate(void *pointer, size_t size, size_t align) {
	// deallocate() wants the size that was passed to allocate(), not the
	// one that the caller originally asked for.
	getSlabPool().deallocate(pointer, alignUp(size, align));
}

} // extern "C"
