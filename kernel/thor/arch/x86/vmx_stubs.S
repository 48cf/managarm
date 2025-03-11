.section .text

.global vmxVmRun
.global vmxDoVmExit
.extern vmxUpdateHostRsp

# rdi: thor::vmx::Vmcs*
# rsi: guest GPR State
# rdx: resume or launch
vmxVmRun:
	# Save callee-saved registers
	pushq %rbx
	pushq %rbp
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15

	movq %rdx, %rbx # RBX is saved by functions
	pushq %rsi

	movq %rsp, %rsi
	call vmxUpdateHostRsp

	# Cannot modify stack pointer from this point
	# Restore GPR state pointer from stack
	movq (%rsp), %rsi

	# Cannot modify flags from this point
	cmpq $1, %rbx

	movq (%rsi), %rax
	movq 8(%rsi), %rbx
	movq 16(%rsi), %rcx
	movq 24(%rsi), %rdx
	movq 40(%rsi), %rdi
	movq 48(%rsi), %rbp
	movq 56(%rsi), %r8
	movq 64(%rsi), %r9
	movq 72(%rsi), %r10
	movq 80(%rsi), %r11
	movq 88(%rsi), %r12
	movq 96(%rsi), %r13
	movq 104(%rsi), %r14
	movq 112(%rsi), %r15
	movq 32(%rsi), %rsi

	je .Ldo_resume
	vmlaunch

	# When vmlaunch errors it doesn't go to kVmcsHostRip (vmxDoVmExit)
	# so we have to jump there manually
	jmp vmxDoVmExit

.Ldo_resume:
	vmresume

vmxDoVmExit:
	pushq %rsi
	movq 8(%rsp), %rsi

	movq %rax, (%rsi)
	movq %rbx, 8(%rsi)
	movq %rcx, 16(%rsi)
	movq %rdx, 24(%rsi)
	movq %rdi, 40(%rsi)
	movq %rbp, 48(%rsi)
	movq %r8, 56(%rsi)
	movq %r9, 64(%rsi)
	movq %r10, 72(%rsi)
	movq %r11, 80(%rsi)
	movq %r12, 88(%rsi)
	movq %r13, 96(%rsi)
	movq %r14, 104(%rsi)
	movq %r15, 112(%rsi)

	popq %rsi # Guest RSI
	popq %r15 # Host RSI (guest GPR state)
	movq %rsi, 32(%r15)

	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rbp
	popq %rbx

	pushfq
	popq %rax
	retq

	.section .note.GNU-stack,"",%progbits
