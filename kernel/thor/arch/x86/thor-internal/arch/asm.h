#pragma once

// This file is included from assembly.
// Use only preprocessor definitions here.

// GS segment (= AssemblyCpuData) offsets.
#define THOR_GS_SELF 0x0
#define THOR_GS_EXECUTOR 0x8
#define THOR_GS_SYSCALL_STACK 0x10
#define THOR_GS_ISEQ_PTR 0x18

#define THOR_EXECUTOR_UAR 0x18
