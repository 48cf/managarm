#pragma once

#include <assert.h>
#include <frg/array.hpp>
#include <stddef.h>
#include <stdint.h>

namespace common::x86 {

enum {
	kCpuIndexFeatures = 1,
	kCpuIndexStructuredExtendedFeaturesEnum = 7,
	kCpuIndexExtendedFeatures = 0x80000001
};

enum {
	// Normal features, EDX register
	kCpuFlagVmx = 1 << 5,
	kCpuFlagPat = 1 << 16,

	// Structured extended features enumeration, EBX register
	kCpuFlagFsGsBase = 1,

	// Extendend features, EDX register
	kCpuFlagSyscall = 0x800,
	kCpuFlagNx = 0x100000,
	kCpuFlagLongMode = 0x20000000
};

inline frg::array<uint32_t, 4> cpuid(uint32_t eax, uint32_t ecx = 0) {
	frg::array<uint32_t, 4> out;
	asm volatile("cpuid"
	             : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3])
	             : "a"(eax), "c"(ecx));
	return out;
}

enum {
	kMsrLocalApicBase = 0x0000001B,
	kMsrFeatureControl = 0x0000003A,
	kMsrPAT = 0x00000277,
	kMsrVmxBasic = 0x00000480,
	kMsrVmxPinBasedCtls = 0x00000481,
	kMsrVmxProcBasedCtls = 0x00000482,
	kMsrVmxExitCtls = 0x00000483,
	kMsrVmxEntryCtls = 0x00000484,
	kMsrVmxMisc = 0x00000485,
	kMsrVmxCr0Fixed0 = 0x00000486,
	kMsrVmxCr0Fixed1 = 0x00000487,
	kMsrVmxCr4Fixed0 = 0x00000488,
	kMsrVmxCr4Fixed1 = 0x00000489,
	kMsrVmxVmcsEnum = 0x0000048A,
	kMsrVmxProcBasedCtls2 = 0x0000048B,
	kMsrVmxEptVpidCap = 0x0000048C,
	kMsrVmxTruePinBasedCtls = 0x0000048D,
	kMsrVmxTrueProcBasedCtls = 0x0000048E,
	kMsrVmxTrueExitCtls = 0x0000048F,
	kMsrVmxTrueEntryCtls = 0x00000490,
	kMsrIa32TscDeadline = 0x000006E0,
	kMsrEfer = 0xC0000080,
	kMsrStar = 0xC0000081,
	kMsrLstar = 0xC0000082,
	kMsrFmask = 0xC0000084,
	kMsrIndexFsBase = 0xC0000100,
	kMsrIndexGsBase = 0xC0000101,
	kMsrIndexKernelGsBase = 0xC0000102,
	kMsrIndexVmCr = 0xC0010114,
};

enum { kMsrSyscallEnable = 1 };

enum {
	kFeatureControlLock = 1 << 0,
	kFeatureControlVmxonInSmx = 1 << 1,
	kFeatureControlVmxonOutsideSmx = 1 << 2,
};

enum {
	kVmxPinBasedExternalInterruptExiting = 1 << 0,
	kVmxPinBasedNmiExiting = 1 << 3,
};

enum {
	kVmxProcBasedCtlInterruptWindowExiting = 1 << 2,
	kVmxProcBasedCtlUseTscOffsetting = 1 << 3,
	kVmxProcBasedCtlHltExiting = 1 << 7,
	kVmxProcBasedCtlInvlpgExiting = 1 << 9,
	kVmxProcBasedCtlMwaitExiting = 1 << 10,
	kVmxProcBasedCtlRdpmcExiting = 1 << 11,
	kVmxProcBasedCtlRdtscExiting = 1 << 12,
	kVmxProcBasedCtlCr3LoadExiting = 1 << 15,
	kVmxProcBasedCtlCr3StoreExiting = 1 << 16,
	kVmxProcBasedCtlCr8LoadExiting = 1 << 19,
	kVmxProcBasedCtlCr8StoreExiting = 1 << 20,
	kVmxProcBasedCtlUseTprShadow = 1 << 21,
	kVmxProcBasedCtlNmiWindowExiting = 1 << 22,
	kVmxProcBasedCtlMovDrExiting = 1 << 23,
	kVmxProcBasedCtlUnconditionalIoExiting = 1 << 24,
	kVmxProcBasedCtlUseIoBitmaps = 1 << 25,
	kVmxProcBasedCtlMonitorTrapFlag = 1 << 27,
	kVmxProcBasedCtlUseMsrBitmaps = 1 << 28,
	kVmxProcBasedCtlMonitorExiting = 1 << 29,
	kVmxProcBasedCtlPauseExiting = 1 << 30,
	kVmxProcBasedCtlActivateSecondaryCtls = 1 << 31,

	kVmxProcBasedCtl2VirtualizeApicAccesses = 1 << 0,
	kVmxProcBasedCtl2EnableEpt = 1 << 1,
	kVmxProcBasedCtl2DescriptorTableExiting = 1 << 2,
	kVmxProcBasedCtl2EnableRdtscp = 1 << 3,
	kVmxProcBasedCtl2VirtualizeX2apicMode = 1 << 4,
	kVmxProcBasedCtl2EnableVpid = 1 << 5,
	kVmxProcBasedCtl2WbinvdExiting = 1 << 6,
	kVmxProcBasedCtl2UnrestrictedGuest = 1 << 7,
	kVmxProcBasedCtl2ApicRegisterVirtualization = 1 << 8,
	kVmxProcBasedCtl2VirtualInterruptDelivery = 1 << 9,
	kVmxProcBasedCtl2PauseLoopExiting = 1 << 10,
	kVmxProcBasedCtl2RdrandExiting = 1 << 11,
	kVmxProcBasedCtl2EnableInvpcid = 1 << 12,
	kVmxProcBasedCtl2EnableVmFunctions = 1 << 13,
	kVmxProcBasedCtl2VmcsShadowing = 1 << 14,
	kVmxProcBasedCtl2EnableEnclsExiting = 1 << 15,
	kVmxProcBasedCtl2RdseedExiting = 1 << 16,
	kVmxProcBasedCtl2EnablePml = 1 << 17,
	kVmxProcBasedCtl2EptViolation = 1 << 18,
	kVmxProcBasedCtl2ConcealVmxFromPt = 1 << 19,
	kVmxProcBasedCtl2EnableXsavesXrstors = 1 << 20,
	kVmxProcBasedCtl2PasidTranslation = 1 << 21,
	kVmxProcBasedCtl2ModeBasedExecuteControlForEpt = 1 << 22,
	kVmxProcBasedCtl2SubpageWritePermissionsForEpt = 1 << 23,
	kVmxProcBasedCtl2IntelPtUsesGuestPhysicalAddresses = 1 << 24,
	kVmxProcBasedCtl2UseTscScaling = 1 << 25,
	kVmxProcBasedCtl2EnableUserWaitAndPause = 1 << 26,
	kVmxProcBasedCtl2EnablePconfig = 1 << 27,
	kVmxProcBasedCtl2EnableEnclvExiting = 1 << 28,
	kVmxProcBasedCtl2VmmBusLockDetection = 1 << 30,
	kVmxProcBasedCtl2InstructionTimeout = 1 << 31,
};

enum {
	kEptCapExecuteOnlyTranslation = 1 << 0,
	kEptCapPageWalkLength4 = 1 << 6,
	kEptCapPageWalkLength5 = 1 << 7,
	kEptCapWriteBackMemoryType = 1 << 14,
	kEptCapAccessedAndDirtyFlags = 1 << 21,
};

inline void xsave(uint8_t *area, uint64_t rfbm) {
	assert(!((uintptr_t)area & 0x3F));

	uintptr_t low = rfbm & 0xFFFFFFFF;
	uintptr_t high = (rfbm >> 32) & 0xFFFFFFFF;
	asm volatile("xsave %0" : : "m"(*area), "a"(low), "d"(high) : "memory");
}

inline void xrstor(uint8_t *area, uint64_t rfbm) {
	assert(!((uintptr_t)area & 0x3F));

	uintptr_t low = rfbm & 0xFFFFFFFF;
	uintptr_t high = (rfbm >> 32) & 0xFFFFFFFF;
	asm volatile("xrstor %0" : : "m"(*area), "a"(low), "d"(high) : "memory");
}

inline void wrmsr(uint32_t index, uint64_t value) {
	uint32_t low = value;
	uint32_t high = value >> 32;
	asm volatile("wrmsr" : : "c"(index), "a"(low), "d"(high) : "memory");
}

inline uint64_t rdmsr(uint32_t index) {
	uint32_t low, high;
	asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(index) : "memory");
	return ((uint64_t)high << 32) | (uint64_t)low;
}

inline void wrxcr(uint32_t index, uint64_t value) {
	uint32_t low = value;
	uint32_t high = value >> 32;
	asm volatile("xsetbv" : : "c"(index), "a"(low), "d"(high) : "memory");
}

inline uint64_t rdxcr(uint32_t index) {
	uint32_t low, high;
	asm volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(index) : "memory");
	return ((uint64_t)high << 32) | (uint64_t)low;
}

inline uint8_t ioInByte(uint16_t port) {
	register uint16_t in_port asm("dx") = port;
	register uint8_t out_value asm("al");
	asm volatile("inb %%dx, %%al" : "=r"(out_value) : "r"(in_port));
	return out_value;
}

inline uint16_t ioInShort(uint16_t port) {
	register uint16_t in_port asm("dx") = port;
	register uint16_t out_value asm("ax");
	asm volatile("inw %%dx, %%ax" : "=r"(out_value) : "r"(in_port));
	return out_value;
}

inline void ioPeekMultiple(uint16_t port, uint16_t *dest, size_t count) {
	asm volatile("cld\n"
	             "\trep insw"
	             :
	             : "d"(port), "D"(dest), "c"(count));
}

inline void ioOutByte(uint16_t port, uint8_t value) {
	register uint16_t in_port asm("dx") = port;
	register uint8_t in_value asm("al") = value;
	asm volatile("outb %%al, %%dx" : : "r"(in_port), "r"(in_value));
}

} // namespace common::x86
