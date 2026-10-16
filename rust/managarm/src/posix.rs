use std::{
    mem::{ManuallyDrop, MaybeUninit},
    sync::LazyLock,
};

use hel::Handle;

bragi::include_binding!(mod bindings = "posix.rs");

pub use managarm_core::data::PosixSupercall;
use managarm_core::data::ManagarmProcessData;

static PROCESS_DATA: LazyLock<ManagarmProcessData> = LazyLock::new(|| {
    // TODO: Create wrapper in `hel`.
    let mut process_data: MaybeUninit<ManagarmProcessData> = MaybeUninit::uninit();
    let result = unsafe {
        hel_sys::helSyscall2(
            hel_sys::kHelCallSuper as i32 + PosixSupercall::GetProcessData as u32 as i32,
            process_data.as_mut_ptr() as u64,
            std::mem::size_of::<ManagarmProcessData>() as u64,
        )
    };

    if result != hel_sys::kHelErrNone as i32 {
        let error_string = unsafe { hel_sys::_helErrorString(result) };
        let error_cstr = unsafe { std::ffi::CStr::from_ptr(error_string) };

        panic!("Failed to get process data: {error_cstr:?}");
    }

    // SAFETY: If the syscall returns kHelErrNone, the process data
    // was successfully initialized by the kernel.
    unsafe { process_data.assume_init() }
});

pub fn posix_lane_handle() -> &'static Handle {
    static POSIX_LANE_HANDLE: LazyLock<ManuallyDrop<Handle>> = LazyLock::new(|| unsafe {
        ManuallyDrop::new(
            Handle::from_raw(PROCESS_DATA.posix_lane)
                .clone_handle()
                .expect("Failed to clone posix lane handle"),
        )
    });

    &POSIX_LANE_HANDLE
}

pub fn mbus_lane_handle() -> &'static Handle {
    static MBUS_LANE_HANDLE: LazyLock<ManuallyDrop<Handle>> = LazyLock::new(|| unsafe {
        ManuallyDrop::new(
            Handle::from_raw(PROCESS_DATA.mbus_lane)
                .clone_handle()
                .expect("Failed to clone mbus lane handle"),
        )
    });

    &MBUS_LANE_HANDLE
}

pub fn hierarchy_handle() -> &'static Handle {
    static HIERARCHY_HANDLE: LazyLock<ManuallyDrop<Handle>> = LazyLock::new(|| unsafe {
        ManuallyDrop::new(
            Handle::from_raw(PROCESS_DATA.hierarchy_handle)
                .clone_handle()
                .expect("Failed to clone hierarchy handle"),
        )
    });

    &HIERARCHY_HANDLE
}
