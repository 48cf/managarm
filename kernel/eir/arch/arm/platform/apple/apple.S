.section .text.init
.global eirEntry

.extern eirStackTop
.extern eirExcVectors
.extern eirRelocate
.extern eirAppleMain

eirEntry:
	b .L1
	nop

.quad 0x0 // Image offset
.quad 0 // Image size (filled in at a later build stage)
.quad 2 // 4K pages
.quad 0 // Reserved
.quad 0 // Reserved
.quad 0 // Reserved
.quad 0x644d5241 // Magic
.quad 0 // Reserved

.L1:
	// Save DTB pointer in X7
	mov x7, x0

	// Set up the stack
	adrp x1, eirStackTop
	add x1, x1, :lo12:eirStackTop
	mov sp, x1

	mov x2, xzr
	orr x2, x2, #(1 << 29)
	orr x2, x2, #(1 << 28)
	orr x2, x2, #(1 << 23)
	orr x2, x2, #(1 << 22)
	orr x2, x2, #(1 << 20)
	orr x2, x2, #(1 << 11)
	orr x2, x2, #(1 << 12)
	orr x2, x2, #(1 << 2)
	msr sctlr_el1, x2

	// Load vector table
	adrp x0, eirExcVectors
	add x0, x0, :lo12:eirExcVectors
	msr vbar_el1, x0

	// Save the DTB pointer, relocate and jump to platform-specific entry
	adrp x1, eirDtbPtr
	add x1, x1, :lo12:eirDtbPtr
	str x7, [x1]

	bl eirRelocate
	bl eirAppleMain

.halt:
	wfe
	b .halt

.section .note.GNU-stack,"",%progbits
