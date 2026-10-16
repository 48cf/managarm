#![no_std]
#![no_main]

extern crate alloc;

mod allocator;
mod elf;
mod files;
mod ipc;
mod logging;
mod posix;
mod process;
mod servers;
mod vfs;

use alloc::rc::Rc;

use hel::Handle;
use hel::executor::Executor;

#[global_allocator]
static ALLOCATOR: allocator::HelAllocator = allocator::HelAllocator;

#[cfg(target_arch = "x86_64")]
#[unsafe(naked)]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn _start() -> ! {
    core::arch::naked_asm!(
        "xor rbp, rbp",
        "and rsp, -16",
        "call {main}",
        main = sym freya_main,
    )
}

#[cfg(target_arch = "aarch64")]
#[unsafe(naked)]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn _start() -> ! {
    core::arch::naked_asm!(
        "mov x29, xzr",
        "mov x30, xzr",
        "bl {main}",
        main = sym freya_main,
    )
}

#[cfg(target_arch = "riscv64")]
#[unsafe(naked)]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn _start() -> ! {
    core::arch::naked_asm!(
        ".option push",
        ".option norelax",
        "la gp, __global_pointer$",
        ".option pop",
        "mv fp, zero",
        "mv ra, zero",
        "call {main}",
        main = sym freya_main,
    )
}

/// What freya works with once it is running: its own resources and the
/// state shared by the servers it runs.
pub struct Freya {
    pub executor: Executor,
    /// freya's own hierarchy; each server gets a child of it.
    pub hierarchy: Handle,
    /// The client lane to mbus, which every server gets a copy of.
    pub mbus_client: Handle,
    /// The server lane to mbus, which only mbus itself gets.
    pub mbus_server: Handle,
    /// The token that servers asking for `access:thor.hardware` get.
    pub hardware_access: Handle,
    pub servers: process::ServerRegistry,
}

extern "C" fn freya_main(bootstrap: *const hel_sys::HelBootstrapData) -> ! {
    info!("freya: started");

    if bootstrap.is_null() {
        panic!("freya: no bootstrap data was handed over");
    }

    // SAFETY: thor writes the struct into freya's address space before
    // starting the thread, and never touches it again.
    let bootstrap = unsafe { &*bootstrap };

    allocator::init(bootstrap.hierarchy);

    vfs::unpack_initrd(bootstrap.hierarchy, map_initrd(bootstrap));
    vfs::add_device_nodes();
    info!("freya: unpacked the initrd");

    // SAFETY: thor attached these descriptors to freya's universe for freya
    // alone, and nothing else in freya wraps them.
    let freya = unsafe {
        Rc::new(Freya {
            executor: Executor::new().expect("freya: failed to create an executor"),
            hierarchy: Handle::from_raw(bootstrap.hierarchy),
            mbus_client: Handle::from_raw(bootstrap.mbusClientLane),
            mbus_server: Handle::from_raw(bootstrap.mbusServerLane),
            hardware_access: Handle::from_raw(bootstrap.hardwareAccess),
            servers: Default::default(),
        })
    };

    let executor = freya.executor.clone();
    let result = executor.block_on(async move {
        servers::boot(&freya);
        info!("freya: launched the boot servers");
        // Keep serving the servers' lanes and supercalls forever.
        core::future::pending::<()>().await
    });
    panic!("freya: the executor stopped: {:?}", result.err());
}

/// Maps the initrd handed over by thor and returns the image as a slice.
///
/// The mapping is never removed: the returned slice is handed to the unpacker,
/// and thor's view of the image stays valid for freya's lifetime.
fn map_initrd(bootstrap: &hel_sys::HelBootstrapData) -> &'static [u8] {
    info!(
        "freya: initrd is {} bytes at offset {} of handle {}",
        bootstrap.initrdSize, bootstrap.initrdOffset, bootstrap.initrdMemory,
    );

    const PAGE_SIZE: usize = 0x1000;
    // `cpio --create --format=newc`, which is what tools/gen-initrd.py runs.
    const NEWC_MAGIC: &[u8] = b"070701";

    // The view starts at a page boundary below the image, so the offset has to be
    // included in the length that is mapped.
    let length = (bootstrap.initrdOffset + bootstrap.initrdSize).next_multiple_of(PAGE_SIZE);

    let mut pointer = core::ptr::null_mut();
    // SAFETY: the handle names a read-only view of at least `length` bytes.
    let res = unsafe {
        hel_sys::helMapMemory(
            bootstrap.initrdMemory,
            hel_sys::kHelNullHandle as hel_sys::HelHandle,
            core::ptr::null_mut(),
            0,
            length,
            hel_sys::kHelMapProtRead,
            &mut pointer,
        )
    };
    if res != hel_sys::kHelErrNone as hel_sys::HelError {
        panic!("freya: failed to map the initrd, helMapMemory returned {res}");
    }

    // SAFETY: the mapping covers `initrdOffset + initrdSize` bytes, so the first
    // six bytes of the image are within it.
    let magic = unsafe {
        core::slice::from_raw_parts(
            pointer.cast::<u8>().add(bootstrap.initrdOffset),
            NEWC_MAGIC.len(),
        )
    };
    if magic != NEWC_MAGIC {
        panic!("freya: the initrd does not start with a newc CPIO header");
    }

    info!("freya: mapped the initrd at {pointer:p} and found a newc CPIO header");

    // SAFETY: the mapping covers the whole image and outlives freya.
    unsafe {
        core::slice::from_raw_parts(
            pointer.cast::<u8>().add(bootstrap.initrdOffset),
            bootstrap.initrdSize,
        )
    }
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    match info.location() {
        Some(location) => error!(
            "freya: panicked at {}:{}: {}",
            location.file(),
            location.line(),
            info.message(),
        ),
        None => error!("freya: panicked: {}", info.message()),
    }

    unsafe { hel_sys::helPanic(core::ptr::null(), 0) }
}

/// The `alloc` crate for this target was built with `panic = "unwind"`, so it
/// emits a reference to the personality routine that `std`` would normally
/// provide. Freya is built with `panic = "abort"` and never unwinds.
#[unsafe(no_mangle)]
extern "C" fn rust_eh_personality() {}
