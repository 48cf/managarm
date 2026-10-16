//! The parts of the managarm userspace libraries that work without `std`.
#![no_std]

extern crate alloc;

pub mod data;
pub mod mbus;

/// Generated bindings for the managarm protocols.
pub mod protocols {
    bragi::include_binding! {
        pub mod fs = "fs.rs",
        pub mod mbus = "mbus.rs",
        pub mod posix = "posix.rs",
        pub mod svrctl = "svrctl.rs",
    }
}
