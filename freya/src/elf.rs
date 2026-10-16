//! Loading ELF images into the address spaces of the processes freya runs.
//!
//! This is the userspace counterpart of thor's `loadModuleImage()`: each
//! `PT_LOAD` segment is copied into fresh memory, which also zeroes the part
//! past the file contents (`.bss`), and mapped at its link address plus `base`.
//! Nothing is relocated, so a base of zero requires an `ET_EXEC` image.

use alloc::string::String;

use hel::{AllocFlags, Handle, Mapping, MappingFlags};

use crate::vfs::VfsFile;
use crate::warn;

const PAGE_SIZE: usize = 0x1000;

const ET_EXEC: u16 = 2;

const PT_LOAD: u32 = 1;
const PT_DYNAMIC: u32 = 2;
const PT_INTERP: u32 = 3;
const PT_NOTE: u32 = 4;
const PT_PHDR: u32 = 6;
const PT_TLS: u32 = 7;
const PT_GNU_EH_FRAME: u32 = 0x6474_e550;
const PT_GNU_STACK: u32 = 0x6474_e551;
const PT_GNU_RELRO: u32 = 0x6474_e552;

const PF_X: u32 = 1;
const PF_W: u32 = 2;
const PF_R: u32 = 4;

/// What loading an image learned about it, for building the aux vector.
pub struct ImageInfo {
    pub entry: usize,
    pub phdr: usize,
    pub phent: usize,
    pub phnum: usize,
    /// The `PT_INTERP` path, if the image is dynamically linked.
    pub interpreter: Option<String>,
}

fn read_u16(image: &[u8], offset: usize) -> Option<u16> {
    Some(u16::from_le_bytes(image.get(offset..offset + 2)?.try_into().ok()?))
}

fn read_u32(image: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(image.get(offset..offset + 4)?.try_into().ok()?))
}

fn read_u64(image: &[u8], offset: usize) -> Option<usize> {
    Some(u64::from_le_bytes(image.get(offset..offset + 8)?.try_into().ok()?) as usize)
}

/// Loads `file` into `space` at `base`, charging its memory to `hierarchy`.
pub fn load(
    hierarchy: &Handle,
    space: &Handle,
    base: usize,
    file: &VfsFile,
) -> Result<ImageInfo, &'static str> {
    let mapped_length = file.len().next_multiple_of(PAGE_SIZE);
    if mapped_length == 0 {
        return Err("the image is empty");
    }
    // SAFETY: the mapping is private to this function and read-only.
    let mapping: Mapping<u8> = unsafe {
        Mapping::new(file.memory(), None, 0, mapped_length, MappingFlags::READ)
            .map_err(|_| "failed to map the image")?
    };
    // SAFETY: the mapping covers the file, which is file.len() bytes long, and
    // nothing writes to it while it is mapped.
    let image = unsafe {
        let start = mapping.as_ptr().ok_or("failed to map the image")?;
        core::slice::from_raw_parts(start.as_ptr().cast_const(), file.len())
    };

    if image.get(..4) != Some(b"\x7fELF") {
        return Err("the image is not an ELF file");
    }
    let bad = "the ELF header is truncated";
    let e_type = read_u16(image, 16).ok_or(bad)?;
    let e_entry = read_u64(image, 24).ok_or(bad)?;
    let e_phoff = read_u64(image, 32).ok_or(bad)?;
    let e_phentsize = read_u16(image, 54).ok_or(bad)? as usize;
    let e_phnum = read_u16(image, 56).ok_or(bad)? as usize;

    if base == 0 && e_type != ET_EXEC {
        return Err("only ET_EXEC images can be loaded without relocation");
    }

    let mut info = ImageInfo {
        entry: base + e_entry,
        phdr: 0,
        phent: e_phentsize,
        phnum: e_phnum,
        interpreter: None,
    };

    for index in 0..e_phnum {
        let phdr = e_phoff + index * e_phentsize;
        let bad = "a program header is truncated";
        let p_type = read_u32(image, phdr).ok_or(bad)?;
        let p_flags = read_u32(image, phdr + 4).ok_or(bad)?;
        let p_offset = read_u64(image, phdr + 8).ok_or(bad)?;
        let p_vaddr = read_u64(image, phdr + 16).ok_or(bad)?;
        let p_filesz = read_u64(image, phdr + 32).ok_or(bad)?;
        let p_memsz = read_u64(image, phdr + 40).ok_or(bad)?;

        match p_type {
            PT_LOAD => {
                if p_memsz == 0 || p_filesz > p_memsz {
                    return Err("a PT_LOAD segment has inconsistent sizes");
                }
                // A segment has to be readable, and writable-and-executable at
                // the same time is never intended.
                if p_flags & PF_R == 0 || (p_flags & PF_W != 0 && p_flags & PF_X != 0) {
                    return Err("illegal combination of segment permissions");
                }
                let contents = image
                    .get(p_offset..p_offset + p_filesz)
                    .ok_or("a PT_LOAD segment extends past the end of the file")?;

                let virt_address = p_vaddr & !(PAGE_SIZE - 1);
                let virt_length = (p_vaddr + p_memsz - virt_address).next_multiple_of(PAGE_SIZE);

                let memory = hel::allocate_memory(hierarchy, virt_length, AllocFlags::empty(), None)
                    .map_err(|_| "failed to allocate memory for a segment")?;
                {
                    // SAFETY: the mapping is private to this block.
                    let fill: Mapping<u8> = unsafe {
                        Mapping::new(
                            &memory,
                            None,
                            0,
                            virt_length,
                            MappingFlags::READ | MappingFlags::WRITE,
                        )
                        .map_err(|_| "failed to map a segment")?
                    };
                    // SAFETY: the segment's contents end within virt_length.
                    unsafe {
                        let destination =
                            fill.as_ptr().ok_or("failed to map a segment")?.as_ptr();
                        destination
                            .add(p_vaddr - virt_address)
                            .copy_from_nonoverlapping(contents.as_ptr(), contents.len());
                    }
                }

                let mut flags = MappingFlags::FIXED | MappingFlags::READ;
                if p_flags & PF_W != 0 {
                    flags |= MappingFlags::WRITE;
                }
                if p_flags & PF_X != 0 {
                    flags |= MappingFlags::EXECUTE;
                }
                // SAFETY: `space` is the new process's address space.
                unsafe {
                    hel::map_memory(&memory, space, Some(base + virt_address), 0, virt_length, flags)
                }
                .map_err(|_| "failed to map a segment into the address space")?;
            }
            PT_INTERP => {
                let path = image
                    .get(p_offset..p_offset + p_filesz)
                    .ok_or("PT_INTERP extends past the end of the file")?;
                let path = path.split(|&byte| byte == 0).next().unwrap_or_default();
                info.interpreter = Some(
                    String::from_utf8(path.into()).map_err(|_| "PT_INTERP is not UTF-8")?,
                );
            }
            PT_PHDR => info.phdr = base + p_vaddr,
            PT_DYNAMIC | PT_TLS | PT_NOTE | PT_GNU_EH_FRAME | PT_GNU_STACK | PT_GNU_RELRO => {}
            other => warn!("freya: unexpected program header of type {other:#x}"),
        }
    }

    Ok(info)
}
