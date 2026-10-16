use core::alloc::{GlobalAlloc, Layout};
use core::ffi::c_void;

unsafe extern "C" {
    fn helHeapInitialize(hierarchy: hel_sys::HelHandle);
    fn helHeapAllocate(size: usize, align: usize) -> *mut c_void;
    fn helHeapDeallocate(pointer: *mut c_void, size: usize, align: usize);
}

pub struct HelAllocator;

unsafe impl GlobalAlloc for HelAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        unsafe { helHeapAllocate(layout.size(), layout.align()) }.cast()
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe { helHeapDeallocate(pointer.cast(), layout.size(), layout.align()) };
    }
}

/// Sets the hierarchy that heap memory is charged to. Must be called before the
/// first allocation.
pub fn init(hierarchy: hel_sys::HelHandle) {
    // SAFETY: freya is single-threaded, so nothing allocates concurrently.
    unsafe { helHeapInitialize(hierarchy) }
}
