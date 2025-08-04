#pragma once

#include <frg/dyn_array.hpp>

#include <thor-internal/irq.hpp>
#include <thor-internal/types.hpp>

namespace thor {

struct ImsicContext;

struct Imsic {
	Imsic(PhysicalAddr base, uint32_t hartIndexBits, uint32_t groupIndexBits)
	: base{base}, hartIndexBits{hartIndexBits}, groupIndexBits{groupIndexBits} {}

	// TODO: Store a dyn_array of all contexts instead of just the BSP's context.
	ImsicContext *bspContext{nullptr};
	PhysicalAddr base;
	uint32_t hartIndexBits;
	uint32_t groupIndexBits;
};

// Per-CPU IMSIC context.
struct ImsicContext {
	ImsicContext(size_t numIrqs, uint32_t hartIndex)
	: irqs{numIrqs, *kernelAlloc}, hartIndex{hartIndex} {}

	size_t findFreeIndex() const {
		for (size_t i = 1; i < irqs.size(); i++) {
			if (irqs[i] == nullptr)
				return i;
		}
		return 0;
	}

	frg::ticket_spinlock irqsLock;
	frg::dyn_array<IrqPin *, KernelAlloc> irqs;

	uint32_t hartIndex;
};

Imsic *getImsicFromPhandle(uint32_t imsicPhandle);
MsiPin *allocateImsicMsi(frg::string<KernelAlloc> name, Imsic *imsic);

} // namespace thor
