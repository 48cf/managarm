//! The structs that a process manager writes into the processes it runs, and
//! the supercall numbers through which they ask for them.
//!
//! These mirror `protocols/posix/data.hpp`, `protocols/posix/supercalls.hpp`
//! and `protocols/svrctl/supercalls.hpp`.

/// Supercall numbers of the POSIX protocol, i.e. offsets from `kHelCallSuper`.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PosixSupercall {
    GetProcessData = 1,
    Fork = 2,
    Execve = 3,
    Exit = 4,
    SigKill = 5,
    SigRestore = 6,
    SigMask = 7,
    SigRaise = 8,
    Clone = 9,
    AnonAllocate = 10,
    AnonDeallocate = 11,
    SigAltStack = 12,
    SigSuspend = 13,
    GetTid = 14,
    SigGetPending = 15,
    SigTimedWait = 16,
    ThreadExit = 17,
    Cancel = 18,
}

/// Supercall numbers of the svrctl protocol, i.e. offsets from `kHelCallSuper`.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SvrctlSupercall {
    GetServerData = 64,
}

/// What `superGetProcessData` writes back: `posix::ManagarmProcessData`.
///
/// The pointers are addresses in the receiving process, which is why they
/// are plain integers here.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct ManagarmProcessData {
    pub posix_lane: hel_sys::HelHandle,
    pub mbus_lane: hel_sys::HelHandle,
    pub thread_page: usize,
    pub file_table: usize,
    pub clock_tracker_page: usize,
    pub hierarchy_handle: hel_sys::HelHandle,
}

/// What `superGetServerData` writes back: `svrctl::ManagarmServerData`.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct ManagarmServerData {
    pub hardware_access: hel_sys::HelHandle,
    pub control_lane: hel_sys::HelHandle,
}

impl ManagarmProcessData {
    pub fn as_bytes(&self) -> &[u8] {
        // SAFETY: repr(C), all fields are plain integers and there is no
        // padding between them on any of the 64-bit targets.
        unsafe {
            core::slice::from_raw_parts((self as *const Self).cast(), size_of::<Self>())
        }
    }
}

impl ManagarmServerData {
    pub fn as_bytes(&self) -> &[u8] {
        // SAFETY: repr(C) and both fields are plain integers.
        unsafe {
            core::slice::from_raw_parts((self as *const Self).cast(), size_of::<Self>())
        }
    }
}
