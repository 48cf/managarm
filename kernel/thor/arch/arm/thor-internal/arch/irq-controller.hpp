#pragma once

#include <frg/optional.hpp>
#include <thor-internal/dtb/irq.hpp>
#include <thor-internal/irq.hpp>

namespace thor {

struct IrqController : dt::IrqController {
	struct CpuIrq {
		uint32_t cpu;
		uint32_t irq;
	};

	virtual void sendIpi(uint32_t cpuId, uint8_t id) = 0;
	virtual void sendIpiToOthers(uint8_t id) = 0;

	virtual CpuIrq getIrq() = 0;
	virtual void eoi(uint32_t cpuId, uint32_t id) = 0;

	virtual IrqPin *setupIrq(uint32_t irq, TriggerMode trigger) = 0;
	virtual IrqPin *getPin(uint32_t irq) = 0;

	virtual IrqPin *handleFiq() = 0;
};

} // namespace thor
