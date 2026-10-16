//! The in-memory filesystem that freya serves the initrd from.
//!
//! This replaces thor's MFS tree (`thor-internal/module.hpp`). Each regular file
//! becomes a copy-on-write memory object holding its contents, so freya can hand
//! a file's memory straight to a server it launches without copying again, and
//! the initrd image itself is not needed after unpacking.

use alloc::{
    collections::btree_map::BTreeMap,
    string::{String, ToString},
    sync::Arc,
    vec::Vec,
};

use hel::{Handle, Mapping, MappingFlags};
use spin::{Lazy, Mutex};

/// Page size, which `helCopyOnWrite` requires its size to be a multiple of.
const PAGE_SIZE: usize = 0x1000;

#[derive(Default)]
pub struct VfsDirectory {
    entries: Mutex<BTreeMap<String, Arc<VfsNode>>>,
}

pub struct VfsFile {
    memory_handle: Handle,
    length: usize,
}

impl VfsFile {
    /// The memory object holding the file's contents. It is padded up to a page
    /// boundary; only the first [`VfsFile::len`] bytes came from the initrd.
    pub fn memory(&self) -> &Handle {
        &self.memory_handle
    }

    pub fn len(&self) -> usize {
        self.length
    }

    pub fn is_empty(&self) -> bool {
        self.length == 0
    }
}

pub enum VfsNode {
    Directory(VfsDirectory),
    File(VfsFile),
}

static ROOT: Lazy<Arc<VfsNode>> =
    Lazy::new(|| Arc::new(VfsNode::Directory(VfsDirectory::default())));

/// The kernel's shared zero-filled memory object, used as the source every
/// file's copy-on-write memory is branched from.
static ZERO_MEMORY: Lazy<Handle> = Lazy::new(|| {
    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    // SAFETY: `handle` is a valid out-parameter.
    let error = unsafe {
        hel_sys::helObtainHandle(hel_sys::kHelObtainZeroMemory as core::ffi::c_int, &mut handle)
    };
    assert_eq!(
        error,
        hel_sys::kHelErrNone as hel_sys::HelError,
        "freya: failed to obtain the zero memory object",
    );

    // SAFETY: helObtainHandle returned a fresh handle in freya's universe.
    unsafe { Handle::from_raw(handle) }
});

/// Returns the directory a path's last component lives in, creating any missing
/// components on the way.
///
/// Creating rather than requiring them is deliberate: `cpio --create` emits
/// entries in sorted order, so parents happen to precede their children today,
/// and thor's parser relies on that. Nothing in the format guarantees it.
fn parent_directory(parts: &[&str]) -> Arc<VfsNode> {
    let mut current = ROOT.clone();

    for &part in parts {
        if part.is_empty() || part == "." {
            continue;
        }

        let VfsNode::Directory(directory) = &*current else {
            panic!("freya: '{part}' is not a directory");
        };

        let next = directory
            .entries
            .lock()
            .entry(part.to_string())
            .or_insert_with(|| Arc::new(VfsNode::Directory(VfsDirectory::default())))
            .clone();

        current = next;
    }

    current
}

/// Copies `contents` into a fresh copy-on-write memory object and returns it.
fn memory_for(hierarchy: hel_sys::HelHandle, contents: &[u8]) -> Handle {
    // helCopyOnWrite works in whole pages, so the tail of the last page stays
    // zero. That suits an ELF image, whose .bss wants exactly that.
    let length = contents.len().next_multiple_of(PAGE_SIZE);

    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    // SAFETY: the zero memory object is at least as long as any offset/size pair
    // asked of it, and `handle` is a valid out-parameter.
    // No swap space: the servers that implement swap are loaded from these
    // files, so their contents must never depend on swap.
    let error = unsafe {
        hel_sys::helCopyOnWrite(
            hierarchy,
            hel_sys::kHelNullHandle as hel_sys::HelHandle,
            ZERO_MEMORY.handle(),
            0,
            length,
            &mut handle,
        )
    };
    assert_eq!(
        error,
        hel_sys::kHelErrNone as hel_sys::HelError,
        "freya: failed to create copy-on-write memory for an initrd file",
    );

    // SAFETY: helCopyOnWrite returned a fresh handle in freya's universe.
    let memory_handle = unsafe { Handle::from_raw(handle) };

    // The mapping exists only to fill the object in. Unlike the memory object it
    // writes to, it is dropped -- and therefore unmapped -- as soon as the copy
    // is done, so unpacking the initrd does not leave one mapping per file
    // behind in freya's address space.
    let mapping: Mapping<u8> = unsafe {
        Mapping::new(
            &memory_handle,
            None,
            0,
            length,
            MappingFlags::READ | MappingFlags::WRITE,
        )
    }
    .expect("freya: failed to map memory for an initrd file");

    // SAFETY: the mapping is at least `contents.len()` bytes long and is not
    // aliased by anything else.
    unsafe {
        mapping
            .as_ptr()
            .expect("freya: mapping for an initrd file is empty")
            .as_ptr()
            .copy_from_nonoverlapping(contents.as_ptr(), contents.len());
    }

    memory_handle
}

/// Unpacks a newc CPIO archive into the tree.
pub fn unpack_initrd(hierarchy: hel_sys::HelHandle, initrd: &[u8]) {
    for entry in cpio_reader::iter_files(initrd) {
        let parts: Vec<_> = entry.name().split('/').collect();
        let Some((name, parents)) = parts.split_last() else {
            continue;
        };

        // A trailing slash, or the archive's own '.' entry, names a directory
        // that parent_directory() has already created.
        if name.is_empty() || *name == "." {
            parent_directory(&parts);
            continue;
        }

        let parent = parent_directory(parents);
        let VfsNode::Directory(directory) = &*parent else {
            panic!("freya: '{}' is not a directory", entry.name());
        };

        let mode = entry.mode();
        if mode.contains(cpio_reader::Mode::DIRECTORY) {
            // May already exist if a child was listed first.
            directory
                .entries
                .lock()
                .entry(name.to_string())
                .or_insert_with(|| Arc::new(VfsNode::Directory(VfsDirectory::default())));
        } else if mode.contains(cpio_reader::Mode::REGULAR_FILE) {
            let file = VfsNode::File(VfsFile {
                memory_handle: memory_for(hierarchy, entry.file()),
                length: entry.file().len(),
            });

            directory
                .entries
                .lock()
                .insert(name.to_string(), Arc::new(file));
        } else {
            panic!("freya: unsupported file type in initrd entry '{}'", entry.name());
        }
    }
}

/// Resolves a path against the root of the tree.
pub fn lookup(path: &str) -> Option<Arc<VfsNode>> {
    let mut current = ROOT.clone();

    for part in path.split('/') {
        if part.is_empty() || part == "." {
            continue;
        }

        let VfsNode::Directory(directory) = &*current else {
            return None;
        };

        let next = directory.entries.lock().get(part).cloned()?;
        current = next;
    }

    Some(current)
}
