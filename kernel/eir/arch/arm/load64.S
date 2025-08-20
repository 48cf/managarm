.section .bss
.align 16
.global eirStackTop
eirStackBottom:
	.skip 0x100000 // reserve 1 MiB for the stack
eirStackTop:

.section .text
.global eirEnterKernel
eirEnterKernel:
	dsb ishst; tlbi vmalle1is; dsb ish; isb

	// x0 -> TTBR0 ptr
	// x1 -> TTBR1 ptr
	// x2 -> thor entry point
	// x3 -> thor stack pointer

	// mrs x8, hcr_el2
	// tbnz x8, #34, 2f

	// Set page table pointers
	msr TTBR0_EL1, x0
	msr TTBR1_EL1, x1
	dsb ishst; tlbi vmalle1is; dsb ish; isb

	// Enable the MMU
	mrs x0, SCTLR_EL1
	orr x0, x0, #1
	msr SCTLR_EL1, x0

	dsb sy
	isb

	b 3f

// 2:
// 	msr s3_5_c2_c0_0, x0 // TTBR0_EL12
//     msr s3_5_c2_c0_1, x1 // TTBR1_EL12
// 	dsb ishst; tlbi vmalle1is; dsb ish; isb
// 
// 	// SCTLR_EL12
// 	mrs x0, sctlr_el1
// 	orr x0, x0, #1
// 	msr s3_5_c1_c0_0, x0
// 
//     dsb sy
//     isb

3:
	mov sp, x3
	br x2

#ifndef __clang__
	.section .note.GNU-stack,"",%progbits
#endif
