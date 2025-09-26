//! A Rust wrapper for Hel.
#![allow(incomplete_features)]
#![feature(maybe_uninit_slice)]
#![feature(generic_const_exprs)]
#![feature(local_waker)]

pub mod executor;
pub mod handle;
pub mod mapping;
pub mod queue;
pub mod result;
pub mod submission;

use std::time::Duration;

pub use executor::{block_on, spawn};
pub use handle::Handle;
pub use mapping::{Mapping, MappingFlags};
pub use queue::Queue;
pub use result::{Error, Result};
pub use submission::{
    action::{
        Accept, Offer, PullDescriptor, PushDescriptor, ReceiveBuffer, ReceiveInline, SendBuffer,
    },
    sleep_for, sleep_until, submit_async,
};

pub fn create_stream(attach_credentials: bool) -> Result<(Handle, Handle)> {
    let mut handles = [0; 2];

    result::hel_check(unsafe {
        hel_sys::helCreateStream(&mut handles[0], &mut handles[1], attach_credentials as _)
    })
    .map(|_| unsafe { (Handle::from_raw(handles[0]), Handle::from_raw(handles[1])) })
}

/// A time value in nanoseconds since boot.
#[repr(transparent)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Time(u64);

impl Time {
    /// Creates a new [`Time`] instance representing the current time
    /// since boot in nanoseconds.
    pub fn new_since_boot() -> Result<Self> {
        let mut nanos = 0;

        result::hel_check(unsafe { hel_sys::helGetClock(&mut nanos) }).map(|_| Self(nanos))
    }

    /// Creates a new [`Time`] instance from the given number of
    /// nanoseconds since boot.
    pub fn from_nanos(nanos: u64) -> Self {
        Self(nanos)
    }

    /// Returns the value of the clock in nanoseconds since boot.
    pub fn nanos(&self) -> u64 {
        self.0
    }
}

impl std::ops::Add<Duration> for Time {
    type Output = Self;

    fn add(self, rhs: Duration) -> Self::Output {
        Self(self.0 + rhs.as_nanos() as u64)
    }
}

impl std::ops::Sub<Duration> for Time {
    type Output = Self;

    fn sub(self, rhs: Duration) -> Self::Output {
        Self(self.0 - rhs.as_nanos() as u64)
    }
}
