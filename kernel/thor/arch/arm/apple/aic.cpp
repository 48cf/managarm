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

namespace aic {

constexpr arch::bit_register<uint32_t> info{0x4};

namespace regs {

namespace info {

constexpr arch::field<uint32_t, uint32_t> nrIrq{0, 16};

} // namespace info

} // namespace regs

} // namespace aic

} // namespace

namespace thor {

inline frg::array<frg::string_view, 1> dtAicCompatible = {"apple,aic"};

struct AicIrqController : IrqController {
	struct Pin : IrqPin {
		Pin(AicIrqController *ic, uint32_t irq, frg::string<KernelAlloc> name)
		: IrqPin{std::move(name)},
		  _aic{ic},
		  _irq{irq} {
			configure({TriggerMode::level, Polarity::high});
		}

		IrqStrategy program(TriggerMode mode, Polarity polarity) override;

		void mask() override;
		void unmask() override;
		void endOfService() override;

	private:
		void _setMaskState(bool masked);

	private:
		AicIrqController *_aic;
		uint32_t _irq;

		bool _isMasked{false};
	};

	AicIrqController(uintptr_t base, size_t size) : _base(base) {
		auto ptr = KernelVirtualMemory::global().allocate(size);
		for (size_t i = 0; i < size; i += kPageSize) {
			KernelPageSpace::global().mapSingle4k(
			    (VirtualAddr)ptr + i, base + i, page_access::write, CachingMode::mmioNonPosted
			);
		}
		_mmio = arch::mem_space{ptr};

		auto info = _mmio.load(aic::info);

		_nrIrq = info & aic::regs::info::nrIrq;
		_maxIrq = 0x400;
		_nrDie = 1;
		_maxDie = 1;
		_dieStride = sizeof(uint32_t) * _maxIrq;

		size_t startOffset = 0x3000;
		size_t offset = startOffset;
		offset += sizeof(uint32_t) * _maxIrq; // TARGET_CPU

		_swSet = offset;
		offset += sizeof(uint32_t) * (_maxIrq >> 5); // SW_SET
		_swClr = offset;
		offset += sizeof(uint32_t) * (_maxIrq >> 5); // SW_CLR
		_maskSet = offset;
		offset += sizeof(uint32_t) * (_maxIrq >> 5); // MASK_SET
		_maskClr = offset;
		offset += sizeof(uint32_t) * (_maxIrq >> 5); // MASK_CLR
		offset += sizeof(uint32_t) * (_maxIrq >> 5); // HW_STATE

		_dieStride = offset - startOffset;

		auto name = frg::string<KernelAlloc>{*kernelAlloc, "aic@0x"};
		name += frg::to_allocated_string(*kernelAlloc, base, 16);

		for (uint32_t i = 0; i < 6; i++) {
			_fiqs[i] = frg::construct<Pin>(
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
	uintptr_t _base;
	arch::mem_space _mmio;

	IrqPin *_fiqs[6];

	size_t _nrIrq;
	size_t _maxIrq;
	size_t _nrDie;
	size_t _maxDie;
	size_t _dieStride;
	size_t _swSet;
	size_t _swClr;
	size_t _maskSet;
	size_t _maskClr;
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

#define AIC_SW_SET 0x4000
#define AIC_SW_CLR 0x4080
#define AIC_MASK_SET 0x4100
#define AIC_MASK_CLR 0x4180

/*

static void aic_irq_mask(struct irq_data *d)
{
    irq_hw_number_t hwirq = irqd_to_hwirq(d);
    struct aic_irq_chip *ic = irq_data_get_irq_chip_data(d);

    u32 off = AIC_HWIRQ_DIE(hwirq) * ic->info.die_stride;
    u32 irq = AIC_HWIRQ_IRQ(hwirq);

    aic_ic_write(ic, ic->info.mask_set + off + MASK_REG(irq), MASK_BIT(irq));
}

static void aic_irq_unmask(struct irq_data *d)
{
    irq_hw_number_t hwirq = irqd_to_hwirq(d);
    struct aic_irq_chip *ic = irq_data_get_irq_chip_data(d);

    u32 off = AIC_HWIRQ_DIE(hwirq) * ic->info.die_stride;
    u32 irq = AIC_HWIRQ_IRQ(hwirq);

    aic_ic_write(ic, ic->info.mask_clr + off + MASK_REG(irq), MASK_BI3(irq));
}

*/

void AicIrqController::Pin::mask() {
	_isMasked = true;
	_setMaskState(true);
}

void AicIrqController::Pin::unmask() {
	_isMasked = false;
	_setMaskState(false);
}

void AicIrqController::Pin::endOfService() {
	if (!_isMasked) {
		unmask();
	}
}

void AicIrqController::Pin::_setMaskState(bool masked) {
	arch::scalar_store<uint32_t>(
	    _aic->_mmio,
	    (masked ? _aic->_maskSet : _aic->_maskClr) + (_irq >> 5) * sizeof(uint32_t),
	    1 << (_irq & 0b11111)
	);
}

void AicIrqController::sendIpi(uint32_t cpuId, uint8_t id) {
	assert(id == 0);
	arch::scalar_store<uint32_t>(_mmio, AIC_IPI_SEND, 1 << cpuId);
}

void AicIrqController::sendIpiToOthers(uint8_t id) {
	(void)id;
	panicLogger() << "thor: AicIrqController::sendIpiToOther should not be called" << frg::endlog;
}

IrqController::CpuIrq AicIrqController::getIrq() {
	auto event = arch::scalar_load<uint32_t>(_mmio, AIC_EVENT);

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
	        + frg::to_allocated_string(*kernelAlloc, _base, 16)
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
		return _fiqs[number];
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
		return _fiqs[kAicTimerHvPhys];
	}

	if (isTimerFiring(cntv_ctl_el0)) {
		return _fiqs[kAicTimerHvVirt];
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
			return _fiqs[kAicTimerGuestPhys];
		}

		if ((enabled & VM_TMR_FIQ_ENABLE_V) && isTimerFiring(cntv_ctl_el02)) {
			return _fiqs[kAicTimerGuestVirt];
		}
	}

	uint64_t pmcr0_el1;
	asm volatile("mrs %0, s3_1_c15_c0_0" : "=r"(pmcr0_el1)); // IMP_APL_PMCR0_EL1

	if (pmcr0_el1 & (1 << 11)) {
		return _fiqs[kAicCpuPmuE];
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
