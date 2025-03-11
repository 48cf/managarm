#include <thor-internal/arch/vmx.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/arch/ept.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/thread.hpp>
#include <x86/machine.hpp>

namespace {
	enum {
		kVmEntryCtlsIa32eModeGuest = 1 << 9,
		kVmEntryCtlsLoadIa32Pat = 1 << 14,
		kVmEntryCtlsLoadIa32Efer = 1 << 15,
	};

	enum {
		kVmExitCtlsHostAddrSpaceSize = 1 << 9,
		kVmExitCtlsSaveIa32Pat = 1 << 18,
		kVmExitCtlsLoadIa32Pat = 1 << 19,
		kVmExitCtlsSaveIa32Efer = 1 << 20,
		kVmExitCtlsLoadIa32Efer = 1 << 21,
	};

	enum {
		kVmcsGuestEsSelector = 0x800,
		kVmcsGuestCsSelector = 0x802,
		kVmcsGuestSsSelector = 0x804,
		kVmcsGuestDsSelector = 0x806,
		kVmcsGuestFsSelector = 0x808,
		kVmcsGuestGsSelector = 0x80A,
		kVmcsGuestLdtrSelector = 0x80C,
		kVmcsGuestTrSelector = 0x80E,

		kVmcsHostEsSelector = 0xC00,
		kVmcsHostCsSelector = 0xC02,
		kVmcsHostSsSelector = 0xC04,
		kVmcsHostDsSelector = 0xC06,
		kVmcsHostFsSelector = 0xC08,
		kVmcsHostGsSelector = 0xC0A,
		kVmcsHostTrSelector = 0xC0C,

		kVmcsEptPointerFull = 0x201A,
		kVmcsEptPointerHigh = 0x201B,

		kVmcsGuestPhysAddrFull = 0x2400,
		kVmcsGuestPhysAddrHigh = 0x2401,

		kVmcsLinkFull = 0x2800,
		kVmcsLinkHigh = 0x2801,
		kVmcsGuestIa32PatFull = 0x2804,
		kVmcsGuestIa32PatHigh = 0x2805,
		kVmcsGuestIa32EferFull = 0x2806,
		kVmcsGuestIa32EferHigh = 0x2807,

		kVmcsHostIa32PatFull = 0x2C00,
		kVmcsHostIa32PatHigh = 0x2C01,
		kVmcsHostIa32EferFull = 0x2C02,
		kVmcsHostIa32EferHigh = 0x2C03,

		kVmcsPinBasedCtls = 0x4000,
		kVmcsPrimaryProcBasedCtls = 0x4002,
		kVmcsExceptionBitmap = 0x4004,
		kVmcsPageFaultErrorCodeMask = 0x4006,
		kVmcsPageFaultErrorCodeMatch = 0x4008,
		kVmcsCr3TargetCount = 0x400A,
		kVmcsVmExitCtls = 0x400C,
		kVmcsVmExitMsrStoreCount = 0x400E,
		kVmcsVmExitMsrLoadCount = 0x4010,
		kVmcsVmEntryCtls = 0x4012,
		kVmcsVmEntryMsrLoadCount = 0x4014,
		kVmcsVmEntryInterruptInfo = 0x4016,
		kVmcsVmEntryExceptionErrorCode = 0x4018,
		kVmcsVmEntryInstructionLength = 0x401A,
		kVmcsSecondaryProcBasedCtls = 0x401E,

		kVmcsVmInstructionError = 0x4400,
		kVmcsExitReason = 0x4402,
		kVmcsVmExitInterruptionInfo = 0x4404,
		kVmcsVmExitInterruptionErrorCode = 0x4406,
		kVmcsIdtVectoringInfo = 0x4408,
		kVmcsIdtVectoringErrorCode = 0x440A,
		kVmcsVmExitInstructionLength = 0x440C,
		kVmcsVmExitInstructionInfo = 0x440E,

		kVmcsGuestEsLimit = 0x4800,
		kVmcsGuestCsLimit = 0x4802,
		kVmcsGuestSsLimit = 0x4804,
		kVmcsGuestDsLimit = 0x4806,
		kVmcsGuestFsLimit = 0x4808,
		kVmcsGuestGsLimit = 0x480A,
		kVmcsGuestLdtrLimit = 0x480C,
		kVmcsGuestTrLimit = 0x480E,
		kVmcsGuestGdtrLimit = 0x4810,
		kVmcsGuestIdtrLimit = 0x4812,
		kVmcsGuestEsAccessRights = 0x4814,
		kVmcsGuestCsAccessRights = 0x4816,
		kVmcsGuestSsAccessRights = 0x4818,
		kVmcsGuestDsAccessRights = 0x481A,
		kVmcsGuestFsAccessRights = 0x481C,
		kVmcsGuestGsAccessRights = 0x481E,
		kVmcsGuestLdtrAccessRights = 0x4820,
		kVmcsGuestTrAccessRights = 0x4822,
		kVmcsGuestInterruptibility = 0x4824,
		kVmcsGuestActivityState = 0x4826,
		kVmcsGuestSysenterCs = 0x482A,

		kVmcsHostIa32SysenterCs = 0x4C00,

		kVmcsCr0Mask = 0x6000,
		kVmcsCr4Mask = 0x6002,
		kVmcsCr0Shadow = 0x6004,
		kVmcsCr4Shadow = 0x6006,
		kVmcsCr3Target0 = 0x6008,
		kVmcsCr3Target1 = 0x600A,
		kVmcsCr3Target2 = 0x600C,
		kVmcsCr3Target3 = 0x600E,

		kVmcsExitQualification = 0x6400,
		kVmcsIoRcx = 0x6402,
		kVmcsIoRsi = 0x6404,
		kVmcsIoRdi = 0x6406,
		kVmcsIoRip = 0x6408,
		kVmcsGuestLinearAddress = 0x640A,

		kVmcsGuestCr0 = 0x6800,
		kVmcsGuestCr3 = 0x6802,
		kVmcsGuestCr4 = 0x6804,
		kVmcsGuestEsBase = 0x6806,
		kVmcsGuestCsBase = 0x6808,
		kVmcsGuestSsBase = 0x680A,
		kVmcsGuestDsBase = 0x680C,
		kVmcsGuestFsBase = 0x680E,
		kVmcsGuestGsBase = 0x6810,
		kVmcsGuestLdtrBase = 0x6812,
		kVmcsGuestTrBase = 0x6814,
		kVmcsGuestGdtrBase = 0x6816,
		kVmcsGuestIdtrBase = 0x6818,
		kVmcsGuestDr7 = 0x681A,
		kVmcsGuestRsp = 0x681C,
		kVmcsGuestRip = 0x681E,
		kVmcsGuestRflags = 0x6820,
		kVmcsGuestPendingDebugExceptions = 0x6822,
		kVmcsGuestSysenterEsp = 0x6824,
		kVmcsGuestSysenterEip = 0x6826,

		kVmcsHostCr0 = 0x6C00,
		kVmcsHostCr3 = 0x6C02,
		kVmcsHostCr4 = 0x6C04,
		kVmcsHostFsBase = 0x6C06,
		kVmcsHostGsBase = 0x6C08,
		kVmcsHostTrBase = 0x6C0A,
		kVmcsHostGdtrBase = 0x6C0C,
		kVmcsHostIdtrBase = 0x6C0E,
		kVmcsHostIa32SysenterEsp = 0x6C10,
		kVmcsHostIa32SysenterEip = 0x6C12,
		kVmcsHostRsp = 0x6C14,
		kVmcsHostRip = 0x6C16,
	};

	enum {
		kVmxExitException = 0,
		kVmxExitExternalInterrupt = 1,
		kVmxExitTripleFault = 2,
		kVmxExitInitSignal = 3,
		kVmxExitStartupIpi = 4,
		kVmxExitIoSmi = 5,
		kVmxExitOtherSmi = 6,
		kVmxExitInterruptWindow = 7,
		kVmxExitNmiWindow = 8,
		kVmxExitTaskSwitch = 9,
		kVmxExitCpuid = 10,
		kVmxExitGetsec = 11,
		kVmxExitHlt = 12,
		kVmxExitInvd = 13,
		kVmxExitInvlpg = 14,
		kVmxExitRdpmc = 15,
		kVmxExitRdtsc = 16,
		kVmxExitRsm = 17,
		kVmxExitVmcall = 18,
		kVmxExitVmclear = 19,
		kVmxExitVmlaunch = 20,
		kVmxExitVmptrld = 21,
		kVmxExitVmptrst = 22,
		kVmxExitVmread = 23,
		kVmxExitVmresume = 24,
		kVmxExitVmwrite = 25,
		kVmxExitVmxoff = 26,
		kVmxExitVmxon = 27,
		kVmxExitCrAccess = 28,
		kVmxExitDrAccess = 29,
		kVmxExitIoInstruction = 30,
		kVmxExitMsrRead = 31,
		kVmxExitMsrWrite = 32,
		kVmxExitEntryFailureInvalidGuestState = 33,
		kVmxExitEntryFailureMsrLoad = 34,
		kVmxExitMwait = 36,
		kVmxExitMonitorTrap = 37,
		kVmxExitMonitor = 39,
		kVmxExitPause = 40,
		kVmxExitEntryFailureMachineCheck = 41,
		kVmxExitTprBelowThreshold = 43,
		kVmxExitApicAccess = 44,
		kVmxExitVirtualizedEoi = 45,
		kVmxExitGdtrIdtrAccess = 46,
		kVmxExitLdtrTrAccess = 47,
		kVmxExitEptViolation = 48,
		kVmxExitEptMisconfig = 49,
		kVmxExitInvept = 50,
		kVmxExitRdtscp = 51,
		kVmxExitPreemptionTimer = 52,
		kVmxExitInvvpid = 53,
		kVmxExitWbinvd = 54,
		kVmxExitXsetbv = 55,
		kVmxExitApicWrite = 56,
		kVmxExitRdrand = 57,
		kVmxExitInvpcid = 58,
		kVmxExitVmfunc = 59,
		kVmxExitEncls = 60,
		kVmxExitRdseed = 61,
		kVmxExitPageModificationLogFull = 62,
		kVmxExitXsaves = 63,
		kVmxExitXrstors = 64,
		kVmxExitPconfig = 65,
		kVmxExitSppRelated = 66,
		kVmxExitUmwait = 67,
		kVmxExitTpause = 68,
		kVmxExitLoadiwkey = 69,
		kVmxExitEnclv = 70,
		kVmxExitEnqcmdPasidTranslationFailure = 72,
		kVmxExitEnqcmdsPasidTranslationFailure = 73,
		kVmxExitBusLock = 74,
		kVmxExitInstructionTimeout = 75,
		kVmxExitSeamcall = 76,
		kVmxExitTdcall = 77,
		kVmxExitRdmsrList = 78,
		kVmxExitWrmsrList = 79,
	};

	inline void vmptrld(PhysicalAddr vmcs) {
		bool success;
		asm volatile("vmptrld %1" : "=@ccnc"(success) : "m"(vmcs) : "memory");
		assert(success && "vmptrld failed");
	}

	inline void vmclear(PhysicalAddr vmcs) {
		bool success;
		asm volatile("vmclear %1" : "=@ccnz"(success) : "m"(vmcs) : "memory");
		assert(success && "vmclear failed");
	}

	inline uint64_t vmread(uint64_t field) {
		bool success;
		uint64_t value;
		asm volatile("vmread %2, %1" : "=@ccnz"(success), "=rm"(value) : "r"(field) : "memory");
		assert(success && "vmread failed");
		return value;
	}

	inline void vmwrite(uint64_t field, uint64_t value) {
		bool success;
		asm volatile("vmwrite %1, %2" : "=@ccnz"(success) : "rm"(value), "r"(field) : "memory");
		assert(success && "vmwrite failed");
	}
}

extern "C" uint64_t vmxVmRun(thor::vmx::Vmcs* vm, thor::GuestState* state, bool resume);
extern "C" uint8_t vmxDoVmExit[]; // Actually a function that cannot be called in the conventional sense, just need the address

extern "C" void vmxUpdateHostRsp(thor::vmx::Vmcs* vm, uintptr_t rsp) {
	vm->updateHostRsp(rsp);
}

namespace thor::vmx {
	bool initialize() {
		infoLogger() << "vmx: Entering VMX operation" << frg::endlog;

		auto vmxonRegion = physicalAllocator->allocate(kPageSize);
		assert(reinterpret_cast<PhysicalAddr>(vmxonRegion) != static_cast<PhysicalAddr>(-1) && "OOM");

		PageAccessor vmxonAccessor{vmxonRegion};
		memset(vmxonAccessor.get(), 0, kPageSize);

		auto control = common::x86::rdmsr(common::x86::kMsrFeatureControl);
		auto expectedBits = common::x86::kFeatureControlLock | common::x86::kFeatureControlVmxonOutsideSmx;
		if ((control & expectedBits) != expectedBits) {
			// Set the lock bit and VMXON outside SMX.
			common::x86::wrmsr(common::x86::kMsrFeatureControl, control | expectedBits);
		}

		uint64_t cr0;
		asm volatile("mov %%cr0, %0" : "=r" (cr0));
		cr0 &= common::x86::rdmsr(common::x86::kMsrVmxCr0Fixed1);
		cr0 |= common::x86::rdmsr(common::x86::kMsrVmxCr0Fixed0);
		asm volatile("mov %0, %%cr0" : : "r" (cr0));

		uint64_t cr4;
		asm volatile("mov %%cr4, %0" : "=r" (cr4));
		cr4 &= common::x86::rdmsr(common::x86::kMsrVmxCr4Fixed1);
		cr4 |= common::x86::rdmsr(common::x86::kMsrVmxCr4Fixed0);
		asm volatile("mov %0, %%cr4" : : "r" (cr4));

		// Enable VMX
		cr4 |= 1 << 13;
		asm volatile("mov %0, %%cr4" : : "r" (cr4));

		// Set VMX revision
		auto vmxRevision = common::x86::rdmsr(common::x86::kMsrVmxBasic);
		*reinterpret_cast<uint32_t*>(vmxonAccessor.get()) = static_cast<uint32_t>(vmxRevision & 0x7FFF'FFFF);

		// Enter VMX operation
		bool success;
		asm volatile("vmxon %1" : "=@ccnc"(success) : "m"(vmxonRegion) : "memory");
		if(success) {
			infoLogger() << "vmx: CPU entered VMX operation" << frg::endlog;
		} else {
			urgentLogger() << "vmx: VMXON failed, this will be a hard error in the future" << frg::endlog;
		}

		return success;
	}

	Vmcs::Vmcs(smarter::shared_ptr<EptSpace> ept) : _space(std::move(ept)) {
		infoLogger() << "vmx: Creating VMCS" << frg::endlog;

		_state = {};
		_vmcs = physicalAllocator->allocate(kPageSize);
		assert(_vmcs != static_cast<PhysicalAddr>(-1) && "OOM");

		PageAccessor vmcsAccessor{_vmcs};
		memset(vmcsAccessor.get(), 0, kPageSize);

		uint32_t vmxRevision = common::x86::rdmsr(common::x86::kMsrVmxBasic);
		*reinterpret_cast<uint32_t*>(vmcsAccessor.get()) = static_cast<uint32_t>(vmxRevision & 0x7FFF'FFFF);

		vmptrld(_vmcs);
		vmwrite(kVmcsLinkFull, static_cast<uint64_t>(-1));

		auto adjustCtls = [](uint32_t msr, uint32_t bits) -> uint32_t {
			auto value = bits;
			auto constraint = common::x86::rdmsr(msr);

			value &= static_cast<uint32_t>(constraint >> 32);
			value |= static_cast<uint32_t>(constraint);

			// Make sure all the bits we want are set
			assert((value & bits) == bits);

			return value;
		};

		// Enable external interrupts and NMIs to cause VM exits
		auto pinBased = adjustCtls(common::x86::kMsrVmxPinBasedCtls,
			common::x86::kVmxPinBasedExternalInterruptExiting |
			common::x86::kVmxPinBasedNmiExiting);
		vmwrite(kVmcsPinBasedCtls, pinBased);

		// Enable VM exits on HLT and port IO instructions
		auto procBased = adjustCtls(common::x86::kMsrVmxProcBasedCtls,
			common::x86::kVmxProcBasedCtlHltExiting |
			common::x86::kVmxProcBasedCtlCr8StoreExiting |
			common::x86::kVmxProcBasedCtlUnconditionalIoExiting |
			common::x86::kVmxProcBasedCtlActivateSecondaryCtls);
		vmwrite(kVmcsPrimaryProcBasedCtls, procBased);

		// Enable EPT and unrestricted guest mode
		auto procBased2 = adjustCtls(common::x86::kMsrVmxProcBasedCtls2,
			common::x86::kVmxProcBasedCtl2EnableEpt |
			common::x86::kVmxProcBasedCtl2UnrestrictedGuest);
		vmwrite(kVmcsSecondaryProcBasedCtls, procBased2);

		// Set up enter and exit controls
		auto exitCtls = adjustCtls(common::x86::kMsrVmxExitCtls,
			kVmExitCtlsHostAddrSpaceSize | kVmExitCtlsLoadIa32Efer | kVmExitCtlsSaveIa32Efer);
		vmwrite(kVmcsVmExitCtls, exitCtls);

		auto entryCtls = adjustCtls(common::x86::kMsrVmxEntryCtls,
			kVmEntryCtlsLoadIa32Efer);
		vmwrite(kVmcsVmEntryCtls, entryCtls);

		// Set up host state on VM exit
		uint64_t cr0, cr4;
		asm volatile("mov %%cr0, %0" : "=r" (cr0));
		asm volatile("mov %%cr4, %0" : "=r" (cr4));
		vmwrite(kVmcsHostCr0, cr0);
		vmwrite(kVmcsHostCr4, cr4);

		common::x86::Gdtr gdtr;
		common::x86::Idtr idtr;
		asm volatile("sgdt %0" : "=m"(gdtr));
		asm volatile("sidt %0" : "=m"(idtr));
		vmwrite(kVmcsHostGdtrBase, reinterpret_cast<uint64_t>(gdtr.pointer));
		vmwrite(kVmcsHostIdtrBase, reinterpret_cast<uint64_t>(idtr.pointer));

		uint32_t entry1 = (gdtr.pointer[kGdtIndexTask * 2] >> 16) & 0xFFFF;
		uint32_t entry2 = (gdtr.pointer[kGdtIndexTask * 2 + 1]) & 0xFF;
		uint32_t entry3 = (gdtr.pointer[kGdtIndexTask * 2 + 1] >> 24) & 0xFF;
		uint32_t entry4 = (gdtr.pointer[kGdtIndexTask * 2 + 2]);
		uint64_t trAddr = static_cast<uint64_t>(entry4) << 32 | entry1 | entry2 << 16 | entry3 << 24;
		vmwrite(kVmcsHostTrBase, trAddr);

		auto efer = common::x86::rdmsr(common::x86::kMsrEfer);
		vmwrite(kVmcsHostIa32EferFull, efer);
		vmwrite(kVmcsHostRip, reinterpret_cast<uint64_t>(vmxDoVmExit));

		auto cr0Fixed0 = common::x86::rdmsr(common::x86::kMsrVmxCr0Fixed0);
		auto cr0Fixed1 = common::x86::rdmsr(common::x86::kMsrVmxCr0Fixed1);
		vmwrite(kVmcsCr0Mask, cr0Fixed0 & cr0Fixed1 & ~((1 << 0) | (1 << 31)));

		auto cr4Fixed0 = common::x86::rdmsr(common::x86::kMsrVmxCr4Fixed0);
		auto cr4Fixed1 = common::x86::rdmsr(common::x86::kMsrVmxCr4Fixed1);
		vmwrite(kVmcsCr4Mask, cr4Fixed0 & cr4Fixed1);

		auto eptCaps = common::x86::rdmsr(common::x86::kMsrVmxEptVpidCap);
		auto eptPointer = static_cast<uint64_t>(_space->rootTable());
		eptPointer |= (4 - 1) << 3; // 4-level paging
		if(eptCaps & common::x86::kEptCapWriteBackMemoryType)
			eptPointer |= 6; // Write-back caching
		if(eptCaps & common::x86::kEptCapAccessedAndDirtyFlags)
			eptPointer |= 1 << 6; // Enable accessed and dirty flags
		vmwrite(kVmcsEptPointerFull, eptPointer);

		if(getGlobalCpuFeatures()->haveXsave){
			auto xsaveRegionSize = getGlobalCpuFeatures()->xsaveRegionSize;
			_hostFstate = reinterpret_cast<uint8_t*>(kernelAlloc->allocate(xsaveRegionSize));
			_guestFstate = reinterpret_cast<uint8_t*>(kernelAlloc->allocate(xsaveRegionSize));
			memset(_hostFstate, 0, xsaveRegionSize);
			memset(_guestFstate, 0, xsaveRegionSize);
		} else {
			_hostFstate = reinterpret_cast<uint8_t*>(kernelAlloc->allocate(512));
			_guestFstate = reinterpret_cast<uint8_t*>(kernelAlloc->allocate(512));
			memset(_hostFstate, 0, 512);
			memset(_guestFstate, 0, 512);
		}

		vmwrite(kVmcsGuestEsLimit, 0xFFFF);
		vmwrite(kVmcsGuestCsLimit, 0xFFFF);
		vmwrite(kVmcsGuestSsLimit, 0xFFFF);
		vmwrite(kVmcsGuestDsLimit, 0xFFFF);
		vmwrite(kVmcsGuestFsLimit, 0xFFFF);
		vmwrite(kVmcsGuestGsLimit, 0xFFFF);
		vmwrite(kVmcsGuestLdtrLimit, 0xFFFF);
		vmwrite(kVmcsGuestTrLimit, 0xFFFF);
		vmwrite(kVmcsGuestGdtrLimit, 0xFFFF);
		vmwrite(kVmcsGuestIdtrLimit, 0xFFFF);

		auto codeAccessRights = 3 | 1 << 4 | 1 << 7;
		auto dataAccessRights = 3 | 1 << 4 | 1 << 7;
		auto ldtrAccessRights = 2 | 1 << 7;
		auto trAccessRights   = 3 | 1 << 7;

		vmwrite(kVmcsGuestEsAccessRights, dataAccessRights);
		vmwrite(kVmcsGuestCsAccessRights, codeAccessRights);
		vmwrite(kVmcsGuestSsAccessRights, dataAccessRights);
		vmwrite(kVmcsGuestDsAccessRights, dataAccessRights);
		vmwrite(kVmcsGuestFsAccessRights, dataAccessRights);
		vmwrite(kVmcsGuestGsAccessRights, dataAccessRights);
		vmwrite(kVmcsGuestLdtrAccessRights, ldtrAccessRights);
		vmwrite(kVmcsGuestTrAccessRights, trAccessRights);
	}

	void Vmcs::updateHostRsp(uintptr_t rsp) {
		if(rsp != _savedHostRsp)
			vmwrite(kVmcsHostRsp, rsp);
		_savedHostRsp = rsp;
	}

	HelVmexitReason Vmcs::run() {
		vmptrld(_vmcs);

		// Save the host state
		uint16_t es, cs, ss, ds, fs, gs, tr;
		asm volatile(
			"mov %%es, %0;"
			"mov %%cs, %1;"
			"mov %%ss, %2;"
			"mov %%ds, %3;"
			"mov %%fs, %4;"
			"mov %%gs, %5;"
			"str %6;"
			: "=r" (es), "=r" (cs), "=r" (ss), "=r" (ds), "=r" (fs), "=r" (gs), "=r" (tr)
		);
		vmwrite(kVmcsHostEsSelector, es);
		vmwrite(kVmcsHostCsSelector, cs);
		vmwrite(kVmcsHostSsSelector, ss);
		vmwrite(kVmcsHostDsSelector, ds);
		vmwrite(kVmcsHostFsSelector, fs);
		vmwrite(kVmcsHostGsSelector, gs);
		vmwrite(kVmcsHostTrSelector, tr);

		uint64_t cr3;
		asm volatile("mov %%cr3, %0" : "=r" (cr3));
		vmwrite(kVmcsHostCr3, cr3);

		auto fsBase = common::x86::rdmsr(common::x86::kMsrIndexFsBase);
		auto gsBase = common::x86::rdmsr(common::x86::kMsrIndexGsBase);
		vmwrite(kVmcsHostFsBase, fsBase);
		vmwrite(kVmcsHostGsBase, gsBase);

		// Clear the VMCS launch state
		vmclear(_vmcs);

		bool resume = false;
		while(true) {
			asm volatile("cli");

			vmptrld(_vmcs);
			if(getGlobalCpuFeatures()->haveXsave){
				common::x86::xsave(_hostFstate, ~0);
				common::x86::xrstor(_guestFstate, ~0);
			} else {
				asm volatile("fxsaveq %0" : : "m" (*_hostFstate));
				asm volatile("fxrstorq %0" : : "m" (*_guestFstate));
			}

			auto rflags = vmxVmRun(this, &_state, resume);
			assert(!(rflags & (1 << 0)));
			resume = true;

			if(getGlobalCpuFeatures()->haveXsave){
				common::x86::xsave(_guestFstate, ~0);
				common::x86::xrstor(_hostFstate, ~0);
			} else {
				asm volatile("fxsaveq %0" : : "m" (*_guestFstate));
				asm volatile("fxrstorq %0" : : "m" (*_hostFstate));
			}

			// VM exits don't restore the GDT limit
			common::x86::Gdtr gdtr;
			asm volatile("sgdt %0": "=m"(gdtr));
			gdtr.limit = 14 * 8;
			asm volatile("lgdt %0": "=m"(gdtr));
			asm volatile("sti");

			auto exitReason = vmread(kVmcsExitReason);
			HelVmexitReason exitInfo = {};
			switch(exitReason) {
			case kVmxExitExternalInterrupt:
				exitInfo.exitReason = kHelVmExitExternalInterrupt;
				return exitInfo;
			case kVmxExitHlt:
				exitInfo.exitReason = kHelVmExitHlt;
				return exitInfo;
			case kVmxExitIoInstruction: {
				auto guestRip = vmread(kVmcsGuestRip);
				auto instructionLength = vmread(kVmcsVmExitInstructionLength);
				auto exitQualification = vmread(kVmcsExitQualification);
				vmwrite(kVmcsGuestRip, guestRip + instructionLength);
				exitInfo.exitReason = kHelVmExitIo;
				exitInfo.address = exitQualification >> 16;
				exitInfo.flags = (exitQualification >> 3) & 1 ? kHelIoRead : kHelIoWrite;
				switch(exitQualification & 0x7) {
					case 0:
						exitInfo.flags |= kHelIoWidth8;
						break;
					case 1:
						exitInfo.flags |= kHelIoWidth16;
						break;
					case 2:
						exitInfo.flags |= kHelIoWidth32;
						break;
					case 3:
						exitInfo.flags |= kHelIoWidth64;
						break;
					default:
						assert(!"Invalid IO width");
				}
				return exitInfo;
			}
			case kVmxExitEptViolation: {
				uint32_t flags = 0;
				auto violationAddress = vmread(kVmcsGuestPhysAddrFull);
				auto exitQualification = vmread(kVmcsExitQualification);
				if(exitQualification & (1 << 1))
					flags |= AddressSpace::kFaultWrite;
				if(exitQualification & (1 << 2))
					flags |= AddressSpace::kFaultExecute;
				auto thisThread = getCurrentThread();
				auto wq = thisThread->pagingWorkQueue();
				auto result = Thread::asyncBlockCurrent(
					_space->handleFault(violationAddress, flags, wq->take()), wq);
				if(result)
					break;
				if(exitQualification & (1 << 0))
					infoLogger() << "vmx: EPT violation due to data read" << frg::endlog;
				if(exitQualification & (1 << 1))
					infoLogger() << "vmx: EPT violation due to data write" << frg::endlog;
				if(exitQualification & (1 << 2))
					infoLogger() << "vmx: EPT violation due to instruction fetch" << frg::endlog;
				infoLogger() << "vmx: Violation address " << frg::hex_fmt{violationAddress} << frg::endlog;
				exitInfo.exitReason = kHelVmExitTranslationFault;
				exitInfo.address = violationAddress;
				exitInfo.flags = flags;
				return exitInfo;
			}
			default:
				urgentLogger() << "vmx: Unhandled VM exit reason " << exitReason << frg::endlog;
				exitInfo.exitReason = kHelVmExitError;
				exitInfo.code = exitReason;
				exitInfo.address = vmread(kVmcsGuestRip);
				return exitInfo;
			}
		}
	}

	void Vmcs::storeRegs(const HelX86VirtualizationRegs *regs) {
		vmptrld(_vmcs);

		memcpy(&_state, regs, sizeof(GuestState));

		vmwrite(kVmcsGuestRsp, regs->rsp);
		vmwrite(kVmcsGuestRip, regs->rip);
		vmwrite(kVmcsGuestRflags, regs->rflags);

		#define setSegment(segment, segment_field) \
			vmwrite(segment_field##Selector, regs->segment.selector); \
			vmwrite(segment_field##Base, regs->segment.base); \
			vmwrite(segment_field##Limit, regs->segment.limit); \
			{ \
				uint32_t attrib = regs->segment.type | (regs->segment.s << 4) | \
					(regs->segment.dpl << 5) | (regs->segment.present << 7) | \
					(regs->segment.avl << 12) | (regs->segment.l << 13) | \
					(regs->segment.db << 14) | (regs->segment.g << 15); \
				vmwrite(segment_field##AccessRights, attrib); \
			}

		setSegment(cs, kVmcsGuestCs);
		setSegment(ds, kVmcsGuestDs);
		setSegment(ss, kVmcsGuestSs);
		setSegment(es, kVmcsGuestEs);
		setSegment(fs, kVmcsGuestFs);
		setSegment(gs, kVmcsGuestGs);

		setSegment(tr, kVmcsGuestTr);
		setSegment(ldt, kVmcsGuestLdtr);

		vmwrite(kVmcsGuestGdtrBase, regs->gdt.base);
		vmwrite(kVmcsGuestGdtrLimit, regs->gdt.limit);

		vmwrite(kVmcsGuestIdtrBase, regs->idt.base);
		vmwrite(kVmcsGuestIdtrLimit, regs->idt.limit);

		auto cr0Fixed0 = common::x86::rdmsr(common::x86::kMsrVmxCr0Fixed0);
		auto cr0Fixed1 = common::x86::rdmsr(common::x86::kMsrVmxCr0Fixed1);
		vmwrite(kVmcsGuestCr0, regs->cr0 | (cr0Fixed0 & cr0Fixed1 & ~((1 << 0) | (1 << 31))));

		vmwrite(kVmcsGuestCr3, regs->cr3);

		auto cr4Fixed0 = common::x86::rdmsr(common::x86::kMsrVmxCr4Fixed0);
		auto cr4Fixed1 = common::x86::rdmsr(common::x86::kMsrVmxCr4Fixed1);
		vmwrite(kVmcsGuestCr4, regs->cr4 | (cr4Fixed0 & cr4Fixed1));

		auto efer = regs->efer;
		if(regs->cr0 & (1 << 31) && regs->efer & (1 << 8)) {
			// Set LMA if CR0.PG and EFER.LME are set
			efer |= 1 << 10;
			// Set TSS type to busy
			auto trAccessRights = vmread(kVmcsGuestTrAccessRights);
			vmwrite(kVmcsGuestTrAccessRights, (trAccessRights & ~0xF) | 0xB);
		}

		auto vmEntryCtls = vmread(kVmcsVmEntryCtls);
		if(efer & (1 << 10) && !(vmEntryCtls & kVmEntryCtlsIa32eModeGuest))
			vmwrite(kVmcsVmEntryCtls, vmEntryCtls | kVmEntryCtlsIa32eModeGuest);
		else if(!(efer & (1 << 10)) && vmEntryCtls & kVmEntryCtlsIa32eModeGuest)
			vmwrite(kVmcsVmEntryCtls, vmEntryCtls & ~kVmEntryCtlsIa32eModeGuest);

		vmwrite(kVmcsGuestIa32EferFull, efer);
	}

	void Vmcs::loadRegs(HelX86VirtualizationRegs *regs) {
		vmptrld(_vmcs);

		memcpy(regs, &_state, sizeof(GuestState));

		regs->rsp = vmread(kVmcsGuestRsp);
		regs->rip = vmread(kVmcsGuestRip);
		regs->rflags = vmread(kVmcsGuestRflags);

		#define getSegment(segment, segment_field) \
			regs->segment.base = vmread(segment_field##Base); \
			regs->segment.limit = vmread(segment_field##Limit); \
			regs->segment.selector = vmread(segment_field##Selector); \
			{ \
				auto seg = vmread(segment_field##AccessRights); \
				regs->segment.type = seg & 0xF; \
				regs->segment.s = (seg >> 4) & 1; \
				regs->segment.dpl = (seg >> 5) & 3; \
				regs->segment.present = (seg >> 7) & 1; \
				regs->segment.avl = (seg >> 12) & 1; \
				regs->segment.l = (seg >> 13) & 1; \
				regs->segment.db = (seg >> 14) & 1; \
				regs->segment.g = (seg >> 15) & 1; \
			}

		getSegment(cs, kVmcsGuestCs);
		getSegment(ds, kVmcsGuestDs);
		getSegment(ss, kVmcsGuestSs);
		getSegment(es, kVmcsGuestEs);
		getSegment(fs, kVmcsGuestFs);
		getSegment(gs, kVmcsGuestGs);

		getSegment(tr, kVmcsGuestTr);
		getSegment(ldt, kVmcsGuestLdtr);

		regs->gdt.base = vmread(kVmcsGuestGdtrBase);
		regs->gdt.limit = vmread(kVmcsGuestGdtrLimit);

		regs->idt.base = vmread(kVmcsGuestIdtrBase);
		regs->idt.limit = vmread(kVmcsGuestIdtrLimit);

		regs->cr0 = vmread(kVmcsGuestCr0);
		regs->cr3 = vmread(kVmcsGuestCr3);
		regs->cr4 = vmread(kVmcsGuestCr4);
		regs->efer = vmread(kVmcsGuestIa32EferFull);
	}

	Vmcs::~Vmcs() {
		vmclear(_vmcs);

		physicalAllocator->free(_vmcs, kPageSize);
	}
}
