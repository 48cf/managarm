//! Server descriptions and the servers that make up the boot.

use alloc::rc::Rc;
use alloc::vec;

use bragi::Struct;
use hel::{Mapping, MappingFlags};
use managarm_core::protocols::svrctl;

use crate::vfs::{self, VfsNode};
use crate::{Freya, error, process};

/// The servers that are started at boot, after mbus, in this order. Their
/// dependencies on each other are resolved over mbus, so only the order in
/// which they are spawned is fixed here.
const BOOT_SERVERS: &[&str] = &[
    "usr/lib/managarm/server/sif.bin",
    "usr/lib/managarm/server/dmalog.bin",
    "usr/lib/managarm/server/kernletcc.bin",
    "usr/lib/managarm/server/posix-devserver.bin",
    "usr/lib/managarm/server/clocktracker.bin",
    "usr/lib/managarm/server/netserver.bin",
    "usr/lib/managarm/server/posix-subsystem.bin",
    "usr/lib/managarm/server/tty-virtio-console.bin",
];

const MBUS_DESCRIPTION: &str = "usr/lib/managarm/server/mbus.bin";

/// Reads a server description (a bragi-encoded `svrctl::Description`, as
/// produced by bakesvr) from the file tree.
pub fn parse_description(path: &str) -> Result<svrctl::Description, &'static str> {
    let node = vfs::lookup(path).ok_or("no such file")?;
    let VfsNode::File(file) = &*node else {
        return Err("not a regular file");
    };

    let mut bytes = vec![0u8; file.len()];
    if !bytes.is_empty() {
        let length = file.len().next_multiple_of(0x1000);
        // SAFETY: the mapping is private to this function and read-only.
        let mapping: Mapping<u8> =
            unsafe { Mapping::new(file.memory(), None, 0, length, MappingFlags::READ) }
                .map_err(|_| "failed to map the file")?;
        // SAFETY: the mapping covers file.len() bytes.
        unsafe {
            let source = mapping.as_ptr().ok_or("failed to map the file")?.as_ptr();
            bytes.copy_from_slice(core::slice::from_raw_parts(source.cast_const(), file.len()));
        }
    }

    let mut description = svrctl::Description::default();
    description
        .decode_body(&mut bragi::io::Cursor::new(&bytes[..]))
        .map_err(|_| "malformed server description")?;
    Ok(description)
}

/// Starts mbus and then the boot servers.
pub fn boot(freya: &Rc<Freya>) {
    let mbus = parse_description(MBUS_DESCRIPTION)
        .unwrap_or_else(|error| panic!("freya: cannot read {MBUS_DESCRIPTION}: {error}"));
    // mbus is the one server that gets the server end of the mbus lane.
    if let Err(error) = process::run_server(freya, &mbus, Some(&freya.mbus_server)) {
        panic!("freya: failed to launch mbus: {error}");
    }

    for &path in BOOT_SERVERS {
        let result = parse_description(path)
            .and_then(|description| process::run_server(freya, &description, None));
        // A server that fails to start is reported but does not stop the
        // boot; the others may still be usable.
        if let Err(error) = result {
            error!("freya: failed to launch {path}: {error}");
        }
    }
}
