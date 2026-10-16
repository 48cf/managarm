//! Logging on top of `helLog`.
//!
//! freya has no libc and therefore no stdio; the kernel log is the only output
//! it has. The buffer used to format messages is a fixed-size array rather than
//! a `String` on purpose: logging has to keep working while the allocator is
//! being brought up and while it is failing.

use core::fmt::{self, Write};

/// Longest message handed to the kernel in a single `helLog` call. Longer
/// messages are split across several calls, which may interleave with output
/// from other threads but never truncates.
const BUFFER_SIZE: usize = 256;

/// A `core::fmt` sink that batches formatted output and flushes it to the
/// kernel log on newlines and whenever the buffer fills up.
pub struct KernelLog {
    severity: hel_sys::HelLogSeverity,
    buffer: [u8; BUFFER_SIZE],
    length: usize,
}

impl KernelLog {
    pub fn new(severity: hel_sys::HelLogSeverity) -> Self {
        Self {
            severity,
            buffer: [0; BUFFER_SIZE],
            length: 0,
        }
    }

    /// Hands whatever has been buffered so far to the kernel.
    pub fn flush(&mut self) {
        if self.length == 0 {
            return;
        }

        // SAFETY: the pointer and length describe the initialised prefix of a
        // buffer that outlives the call. The error is deliberately ignored:
        // there is nowhere left to report a failure to log to.
        unsafe {
            hel_sys::helLog(
                self.severity,
                self.buffer.as_ptr().cast::<core::ffi::c_char>(),
                self.length,
            );
        }

        self.length = 0;
    }
}

impl Drop for KernelLog {
    fn drop(&mut self) {
        self.flush();
    }
}

impl Write for KernelLog {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for &byte in s.as_bytes() {
            if byte == b'\n' {
                self.flush();
                continue;
            }

            if self.length == BUFFER_SIZE {
                self.flush();
            }

            self.buffer[self.length] = byte;
            self.length += 1;
        }

        Ok(())
    }
}

/// Formats a message and writes it to the kernel log at the given severity.
/// Prefer the [`info!`], [`warn!`] and [`error!`] macros over calling this.
pub fn log_fmt(severity: hel_sys::HelLogSeverity, args: fmt::Arguments) {
    let mut log = KernelLog::new(severity);
    // Writing to the kernel log cannot fail in a way we can act on.
    let _ = log.write_fmt(args);
}

#[macro_export]
macro_rules! info {
    ($($arg:tt)*) => {
        $crate::logging::log_fmt(
            ::hel_sys::kHelLogSeverityInfo,
            format_args!($($arg)*),
        )
    };
}

#[macro_export]
macro_rules! warn {
    ($($arg:tt)*) => {
        $crate::logging::log_fmt(
            ::hel_sys::kHelLogSeverityWarning,
            format_args!($($arg)*),
        )
    };
}

#[macro_export]
macro_rules! error {
    ($($arg:tt)*) => {
        $crate::logging::log_fmt(
            ::hel_sys::kHelLogSeverityError,
            format_args!($($arg)*),
        )
    };
}
