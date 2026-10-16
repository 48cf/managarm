//! Creating and controlling threads in other universes and address spaces.
//!
//! This is what a process manager needs to run a program: create a universe
//! and an address space, start a thread in them, and act on what the thread
//! raises by reading and writing its registers before resuming it.

use bitflags::bitflags;

use crate::handle::Handle;
use crate::result::{Result, hel_check};

/// Creates a new, empty universe that threads can be started in.
pub fn create_universe() -> Result<Handle> {
    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    hel_check(unsafe { hel_sys::helCreateUniverse(&mut handle) })?;
    Ok(unsafe { Handle::from_raw(handle) })
}

bitflags! {
    /// Flags for [`create_thread`].
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct ThreadFlags: u32 {
        /// The thread does not run until it is resumed with [`resume`].
        const STOPPED = hel_sys::kHelThreadStopped;
    }
}

/// Creates a thread that runs in `universe` and `space`, starting at `ip` with
/// the stack pointer set to `sp`.
pub fn create_thread(
    universe: &Handle,
    space: &Handle,
    ip: usize,
    sp: usize,
    flags: ThreadFlags,
) -> Result<Handle> {
    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    hel_check(unsafe {
        hel_sys::helCreateThread(
            universe.handle(),
            space.handle(),
            hel_sys::kHelAbiSystemV as hel_sys::HelAbi,
            ip as *mut core::ffi::c_void,
            sp as *mut core::ffi::c_void,
            flags.bits(),
            &mut handle,
        )
    })?;
    Ok(unsafe { Handle::from_raw(handle) })
}

/// Resumes a thread that was created stopped or that stopped to be observed.
pub fn resume(thread: &Handle) -> Result<()> {
    hel_check(unsafe { hel_sys::helResume(thread.handle()) })
}

/// Terminates a thread.
pub fn kill(thread: &Handle) -> Result<()> {
    hel_check(unsafe { hel_sys::helKillThread(thread.handle()) })
}

/// The general purpose registers of a stopped thread, in the order and
/// number of the target's `kHelReg*` constants.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Registers(pub [usize; hel_sys::kHelNumGprs as usize]);

impl Registers {
    /// Loads the general purpose registers of a stopped thread.
    pub fn load(thread: &Handle) -> Result<Self> {
        let mut gprs = [0; hel_sys::kHelNumGprs as usize];
        hel_check(unsafe {
            hel_sys::helLoadRegisters(
                thread.handle(),
                hel_sys::kHelRegsGeneral as i32,
                gprs.as_mut_ptr().cast(),
            )
        })?;
        Ok(Self(gprs))
    }

    /// Stores the general purpose registers of a stopped thread.
    pub fn store(&self, thread: &Handle) -> Result<()> {
        hel_check(unsafe {
            hel_sys::helStoreRegisters(
                thread.handle(),
                hel_sys::kHelRegsGeneral as i32,
                self.0.as_ptr().cast(),
            )
        })
    }

    /// Returns the `index`-th argument of the supercall the thread raised.
    pub fn arg(&self, index: usize) -> usize {
        const ARGS: [u32; 3] = [hel_sys::kHelRegArg0, hel_sys::kHelRegArg1, hel_sys::kHelRegArg2];
        self.0[ARGS[index] as usize]
    }

    /// Sets the values that the thread's supercall returns: the error and up
    /// to three output words.
    pub fn set_result(&mut self, error: hel_sys::HelError, outputs: &[usize]) {
        const OUTS: [u32; 3] = [hel_sys::kHelRegOut0, hel_sys::kHelRegOut1, hel_sys::kHelRegOut2];
        self.0[hel_sys::kHelRegError as usize] = error as usize;
        for (index, value) in outputs.iter().enumerate() {
            self.0[OUTS[index] as usize] = *value;
        }
    }
}

/// Loads the instruction and stack pointer of a stopped thread.
pub fn load_program_registers(thread: &Handle) -> Result<(usize, usize)> {
    let mut pcrs = [0usize; 2];
    hel_check(unsafe {
        hel_sys::helLoadRegisters(
            thread.handle(),
            hel_sys::kHelRegsProgram as i32,
            pcrs.as_mut_ptr().cast(),
        )
    })?;
    Ok((
        pcrs[hel_sys::kHelRegIp as usize],
        pcrs[hel_sys::kHelRegSp as usize],
    ))
}

/// What stopped an observed thread.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Observation {
    Null,
    Breakpoint,
    PageFault,
    Panic,
    Interrupt,
    GeneralFault,
    IllegalInstruction,
    DivByZero,
    /// The thread issued supercall number `n`, i.e. `kHelCallSuper + n`.
    SuperCall(u32),
    /// An observation that this crate does not know about.
    Unknown(u32),
}

impl Observation {
    pub(crate) fn from_raw(raw: u32) -> Self {
        if raw & hel_sys::kHelObserveSuperCall != 0 {
            return Self::SuperCall(raw & !hel_sys::kHelObserveSuperCall);
        }
        match raw {
            hel_sys::kHelObserveNull => Self::Null,
            hel_sys::kHelObserveBreakpoint => Self::Breakpoint,
            hel_sys::kHelObservePageFault => Self::PageFault,
            hel_sys::kHelObservePanic => Self::Panic,
            hel_sys::kHelObserveInterrupt => Self::Interrupt,
            hel_sys::kHelObserveGeneralFault => Self::GeneralFault,
            hel_sys::kHelObserveIllegalInstruction => Self::IllegalInstruction,
            hel_sys::kHelObserveDivByZero => Self::DivByZero,
            other => Self::Unknown(other),
        }
    }
}
