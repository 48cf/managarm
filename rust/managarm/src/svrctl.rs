use hel::Handle;
use std::mem::ManuallyDrop;
use std::sync::LazyLock;

pub use managarm_core::data::SvrctlSupercall;
use managarm_core::data::ManagarmServerData;

static SERVER_DATA: LazyLock<ManagarmServerData> = LazyLock::new(|| {
    let mut server_data = ManagarmServerData::default();
    let result = unsafe {
        hel_sys::helSyscall2(
            hel_sys::kHelCallSuper as i32 + SvrctlSupercall::GetServerData as u32 as i32,
            (&raw mut server_data).addr() as u64,
            std::mem::size_of::<ManagarmServerData>() as u64,
        )
    };

    if result != hel_sys::kHelErrNone as i32 {
        let error_string = unsafe { hel_sys::_helErrorString(result) };
        let error_cstr = unsafe { std::ffi::CStr::from_ptr(error_string) };

        panic!("Failed to get server data: {error_cstr:?}");
    }

    server_data
});

pub fn hardware_access_handle() -> &'static Handle {
    static HANDLE: LazyLock<ManuallyDrop<Handle>> = LazyLock::new(|| unsafe {
        ManuallyDrop::new(Handle::from_raw(SERVER_DATA.hardware_access))
    });

    &HANDLE
}
