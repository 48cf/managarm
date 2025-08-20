#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/arch/gic.hpp>
#include <thor-internal/arch/irq-controller.hpp>
#include <thor-internal/arch/system.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/main.hpp>

namespace {

constexpr size_t kAicIrq = 0;
constexpr size_t kAicFiq = 1;

constexpr size_t kAicTimerHvPhys = 0;
constexpr size_t kAicTimerHvVirt = 1;
constexpr size_t kAicTimerGuestPhys = 2;
constexpr size_t kAicTimerGuestVirt = 3;
constexpr size_t kAicCpuPmuE = 4;
constexpr size_t kAicCpuPmuP = 5;

} // namespace

namespace thor {

inline frg::array<frg::string_view, 1> dtAicCompatible = {"apple,aic"};

struct AicIrqController : IrqController {
	struct Pin : IrqPin {
		Pin(AicIrqController *ic, uint32_t irq, frg::string<KernelAlloc> name)
		: IrqPin{std::move(name)},
		  ic_(ic),
		  irq_(irq) {
			configure({TriggerMode::level, Polarity::high});
		}

		IrqStrategy program(TriggerMode mode, Polarity polarity) override;

		void mask() override;
		void unmask() override;
		void endOfService() override;

	private:
		AicIrqController *ic_;
		uint32_t irq_;

		bool isMasked_{false};
	};

	AicIrqController(uintptr_t base, size_t size) : base_(base) {
		auto ptr = KernelVirtualMemory::global().allocate(size);
		for (size_t i = 0; i < size; i += kPageSize) {
			KernelPageSpace::global().mapSingle4k(
			    (VirtualAddr)ptr + i, base + i, page_access::write, CachingMode::mmioNonPosted
			);
		}
		space_ = arch::mem_space{ptr};

		auto name = frg::string<KernelAlloc>{*kernelAlloc, "aic@0x"};
		name += frg::to_allocated_string(*kernelAlloc, base, 16);

		for (uint32_t i = 0; i < 6; i++) {
			fiqs_[i] = frg::construct<Pin>(
			    *kernelAlloc,
			    this,
			    i,
			    name + frg::string<KernelAlloc>{*kernelAlloc, ":fiq"}
			        + frg::to_allocated_string(*kernelAlloc, i)
			);
		}
	}

	void sendIpi(uint32_t cpuId, uint8_t id) override;
	void sendIpiToOthers(uint8_t id) override;

	CpuIrq getIrq() override;
	void eoi(uint32_t cpuId, uint32_t id) override;

	IrqPin *setupIrq(uint32_t irq, TriggerMode trigger) override;
	IrqPin *getPin(uint32_t irq) override;
	IrqPin *resolveDtIrq(dtb::Cells irq) override;

	IrqPin *handleFiq() override;

private:
	uintptr_t base_;
	arch::mem_space space_;
	IrqPin *fiqs_[6];
};

IrqStrategy AicIrqController::Pin::program(TriggerMode mode, Polarity polarity) {
	assert(mode == TriggerMode::level);
	assert(polarity == Polarity::high);

	unmask();
	return irq_strategy::maskable | irq_strategy::endOfService;
}

#define AIC_EVENT 0x2004
#define AIC_IPI_SEND 0x2008
#define AIC_IPI_ACK 0x200c
#define AIC_IPI_MASK_SET 0x2024
#define AIC_IPI_MASK_CLR 0x2028

#define AIC_EVENT_TYPE_HW 1
#define AIC_EVENT_TYPE_IPI 4
#define AIC_EVENT_IPI_OTHER 1
#define AIC_EVENT_IPI_SELF 2

void AicIrqController::Pin::mask() {
	isMasked_ = true;

	// ...
}

void AicIrqController::Pin::unmask() {
	isMasked_ = false;

	// ...
}

void AicIrqController::Pin::endOfService() {
	if (!isMasked_) {
		unmask();
	}
}

void AicIrqController::sendIpi(uint32_t cpuId, uint8_t id) {
	assert(id == 0);
	arch::scalar_store<uint32_t>(space_, AIC_IPI_SEND, 1 << cpuId);
}

void AicIrqController::sendIpiToOthers(uint8_t id) {
	(void)id;
	panicLogger() << "thor: AicIrqController::sendIpiToOther should not be called" << frg::endlog;
}

IrqController::CpuIrq AicIrqController::getIrq() {
	auto event = arch::scalar_load<uint32_t>(space_, AIC_EVENT);

	auto irq = event & 0xffff;
	auto type = (event >> 16) & 0xffff;

	if (type == AIC_EVENT_TYPE_IPI)
		return {0, 0};

	assert(!"AicIrqController::getIrq - NYI");
}

void AicIrqController::eoi(uint32_t cpuId, uint32_t id) {
	(void)cpuId;
	(void)id;
}

IrqPin *AicIrqController::setupIrq(uint32_t irq, TriggerMode trigger) {
	return frg::construct<AicIrqController::Pin>(
	    *kernelAlloc,
	    this,
	    irq,
	    frg::string<KernelAlloc>{*kernelAlloc, "aic@0x"}
	        + frg::to_allocated_string(*kernelAlloc, base_, 16)
	        + frg::string<KernelAlloc>{*kernelAlloc, ":irq"}
	        + frg::to_allocated_string(*kernelAlloc, irq)
	);
}

IrqPin *AicIrqController::getPin(uint32_t irq) { assert(!"AicIrqController::getPin - NYI"); }

IrqPin *AicIrqController::resolveDtIrq(dtb::Cells irq) {
	uint32_t type;
	uint32_t number;
	uint32_t flags;

	assert(irq.readSlice(type, 0, 1));
	assert(irq.readSlice(number, 1, 1));
	assert(irq.readSlice(flags, 2, 1));

	if (type == kAicFiq) {
		assert(number < 6);
		return fiqs_[number];
	}

	return setupIrq(number, TriggerMode::edge);
}

IrqPin *AicIrqController::handleFiq() {
#define ARCH_TIMER_CTRL_ENABLE (1 << 0)
#define ARCH_TIMER_CTRL_IT_MASK (1 << 1)
#define ARCH_TIMER_CTRL_IT_STAT (1 << 2)

	uint64_t cntp_ctl_el0;
	uint64_t cntv_ctl_el0;
	asm volatile("mrs %0, cntp_ctl_el0" : "=r"(cntp_ctl_el0));
	asm volatile("mrs %0, cntv_ctl_el0" : "=r"(cntv_ctl_el0));

	const auto isTimerFiring = [](uint64_t ctl) {
		return (ctl & ARCH_TIMER_CTRL_ENABLE) != 0 && (ctl & ARCH_TIMER_CTRL_IT_MASK) == 0
		       && (ctl & ARCH_TIMER_CTRL_IT_STAT) != 0;
	};

	if (isTimerFiring(cntp_ctl_el0)) {
		return fiqs_[kAicTimerHvPhys];
	}

	if (isTimerFiring(cntv_ctl_el0)) {
		return fiqs_[kAicTimerHvVirt];
	}

	if (isKernelInEl2()) {
		uint64_t enabled;
		asm volatile("mrs %0, s3_5_c15_c1_3" : "=r"(enabled)); // IMP_APL_VM_TMR_FIQ_ENA_EL2

		uint64_t cntp_ctl_el02;
		uint64_t cntv_ctl_el02;
		asm volatile("mrs %0, s3_5_c14_c2_1" : "=r"(cntp_ctl_el02));
		asm volatile("mrs %0, s3_5_c14_c3_1" : "=r"(cntv_ctl_el02));

#define VM_TMR_FIQ_ENABLE_V (1 << 0)
#define VM_TMR_FIQ_ENABLE_P (1 << 1)

		if ((enabled & VM_TMR_FIQ_ENABLE_P) && isTimerFiring(cntp_ctl_el02)) {
			return fiqs_[kAicTimerGuestPhys];
		}

		if ((enabled & VM_TMR_FIQ_ENABLE_V) && isTimerFiring(cntv_ctl_el02)) {
			return fiqs_[kAicTimerGuestVirt];
		}
	}

	uint64_t pmcr0_el1;
	asm volatile("mrs %0, s3_1_c15_c0_0" : "=r"(pmcr0_el1)); // IMP_APL_PMCR0_EL1

	if (pmcr0_el1 & (1 << 11)) {
		return fiqs_[kAicCpuPmuE];
	}

	return nullptr;
}

namespace {

frg::manual_box<AicIrqController> aic;

initgraph::Task initAic{
    &globalInitEngine,
    "arm.apple.init-aic",
    initgraph::Requires{getDeviceTreeParsedStage(), getBootProcessorReadyStage()},
    initgraph::Entails{getIrqControllerReadyStage()},
    [] {
	    DeviceTreeNode *aicNode = nullptr;
	    getDeviceTreeRoot()->forEach([&](DeviceTreeNode *node) -> bool {
		    if (node->isCompatible(dtAicCompatible)) {
			    aicNode = node;
			    return true;
		    }

		    return false;
	    });

	    if (!aicNode) {
		    return;
	    }

	    infoLogger() << "thor: found the AIC at node \"" << aicNode->path() << "\"" << frg::endlog;

	    auto reg = aicNode->reg()[0];
	    aic.initialize(reg.addr, reg.size);

	    irqController = aic.get();
	    aicNode->associateIrqController(aic.get());
    }
};

} // namespace

} // namespace thor
