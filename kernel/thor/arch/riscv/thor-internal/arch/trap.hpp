#pragma once

#include <thor-internal/arch-generic/cpu.hpp>

namespace thor {

struct IrqPin;

extern "C" void thorExceptionEntry();

void handleRiscvWorkOnExecutor(Executor *executor, Frame *frame);

enum ExternalIrqType {
	none,
	plic,
	imsic,
	aplic,
};

struct ExternalIrq {
	ExternalIrqType type{ExternalIrqType::none};
	void *controller{nullptr};
	// For PLIC: index of the PLIC context.
	// For APLIC: hart index inside the APLIC domain.
	size_t context{~size_t{0}};
};

extern PerCpu<ExternalIrq> riscvExternalIrq;

IrqPin *claimPlicIrq();
IrqPin *claimImsicIrq();
IrqPin *claimAplicIrq();

} // namespace thor
