//! The files that freya serves to the processes it runs, over the fs protocol.
//!
//! These are the files that thor's POSIX emulation provided: the standard
//! streams, which go to the kernel log, the initrd's regular files and
//! directories, and `/dev/urandom`. Each open file has its own lane and its
//! own task serving it.

use alloc::rc::Rc;
use alloc::string::String;
use alloc::sync::Arc;
use alloc::vec;

use hel::executor::Executor;
use hel::{Handle, Mapping, MappingFlags};
use managarm_core::protocols::fs;

use crate::ipc;
use crate::vfs::{VfsFile, VfsNode};
use crate::{error, info, warn};

/// What an open file refers to; kept in the owning process's file table.
pub enum OpenFile {
    Stdio,
    Regular(Arc<VfsNode>),
    Directory(Arc<VfsNode>),
    Urandom,
}

impl OpenFile {
    pub fn is_terminal(&self) -> bool {
        matches!(self, Self::Stdio)
    }

    /// The memory object backing a regular file, for mapping it.
    pub fn memory(&self) -> Option<&Handle> {
        match self {
            Self::Regular(node) => match &**node {
                VfsNode::File(file) => Some(file.memory()),
                _ => None,
            },
            _ => None,
        }
    }
}

/// Upper bound for a single write to a standard stream. Writes are line
/// oriented log output; mlibc's stdio buffers are far smaller than this.
const MAX_STDIO_WRITE: usize = 0x10000;

/// Rights that clients get on a regular file's memory when they map it.
const FILE_MEMORY_RIGHTS: hel_sys::HelRights = hel_sys::kHelRightRead
    | hel_sys::kHelRightAssign
    | hel_sys::kHelRightProvision
    | hel_sys::kHelRightPin
    | hel_sys::kHelRightFence;

fn svr_response(error: fs::Errors) -> fs::SvrResponse {
    let mut response = fs::SvrResponse::default();
    response.set_error(error);
    response
}

/// Serves a file on `lane` until all clients have closed it.
pub async fn serve(executor: Executor, lane: Handle, file: Rc<OpenFile>) {
    let result = match &*file {
        OpenFile::Stdio => serve_stdio(&executor, &lane).await,
        OpenFile::Regular(node) => match &**node {
            VfsNode::File(contents) => serve_regular(&executor, &lane, contents).await,
            _ => unreachable!(),
        },
        OpenFile::Directory(node) => match &**node {
            VfsNode::Directory(_) => serve_directory(&executor, &lane, node).await,
            _ => unreachable!(),
        },
        OpenFile::Urandom => serve_urandom(&executor, &lane).await,
    };
    if let Err(error) = result {
        warn!("freya: stopped serving a file: {error}");
    }
}

async fn serve_stdio(executor: &Executor, lane: &Handle) -> hel::Result<()> {
    let mut line = String::new();

    while let Some((conversation, head)) = ipc::accept(executor, lane).await? {
        let Ok(preamble) = bragi::preamble_from_bytes(&head) else {
            warn!("freya: failed to decode a stdio request preamble");
            return Ok(());
        };

        if preamble.id() == <fs::WriteRequest as bragi::Message>::MESSAGE_ID {
            let Ok(request) = bragi::head_from_bytes::<fs::WriteRequest>(&head) else {
                warn!("freya: could not parse a stdio write request");
                return Ok(());
            };
            ipc::extract_credentials(executor, &conversation).await?;
            let max = (request.size() as usize).min(MAX_STDIO_WRITE);
            let data = ipc::recv_buffer(executor, &conversation, max).await?;

            // Like thor, emit one log message per complete line.
            for &byte in &data {
                if byte == b'\n' {
                    info!("{line}");
                    line.clear();
                } else {
                    line.push(byte as char);
                }
            }

            let mut response = svr_response(fs::Errors::Success);
            response.set_size(data.len() as i64);
            ipc::send_head(executor, &conversation, &response).await?;
        } else if preamble.id() == <fs::CntRequest as bragi::Message>::MESSAGE_ID {
            let request = bragi::head_from_bytes::<fs::CntRequest>(&head).ok();
            if request.map(|r| r.req_type()) == Some(fs::CntReqType::SeekRel) {
                let response = svr_response(fs::Errors::SeekOnPipe);
                ipc::send_head(executor, &conversation, &response).await?;
            } else {
                error!("freya: illegal request type for a stdio file");
                ipc::dismiss(executor, &conversation).await?;
            }
        } else {
            error!("freya: illegal request ID {} for a stdio file", preamble.id());
            ipc::dismiss(executor, &conversation).await?;
        }
    }

    Ok(())
}

async fn serve_regular(executor: &Executor, lane: &Handle, file: &VfsFile) -> hel::Result<()> {
    let mut offset = 0usize;

    while let Some((conversation, head)) = ipc::accept(executor, lane).await? {
        let Ok(preamble) = bragi::preamble_from_bytes(&head) else {
            warn!("freya: failed to decode a regular file request preamble");
            return Ok(());
        };

        if preamble.id() == <fs::ReadRequest as bragi::Message>::MESSAGE_ID {
            let Ok(request) = bragi::head_from_bytes::<fs::ReadRequest>(&head) else {
                warn!("freya: could not parse a regular file read request");
                return Ok(());
            };
            ipc::extract_credentials(executor, &conversation).await?;

            let start = offset.min(file.len());
            let length = (request.size() as usize).min(file.len() - start);
            let mut data = vec![0u8; length];
            if length > 0 {
                read_file(file, start, &mut data)?;
            }
            offset = start + length;

            let response = svr_response(fs::Errors::Success);
            ipc::send_head(executor, &conversation, &response).await?;
            ipc::send(executor, &conversation, &data).await?;
        } else if preamble.id() == <fs::CntRequest as bragi::Message>::MESSAGE_ID {
            let request = bragi::head_from_bytes::<fs::CntRequest>(&head).ok();
            match request.as_ref().map(|r| r.req_type()) {
                Some(fs::CntReqType::SeekAbs) => {
                    let target = request.and_then(|r| r.rel_offset()).unwrap_or(0);
                    offset = usize::try_from(target).unwrap_or(0);
                    let response = svr_response(fs::Errors::Success);
                    ipc::send_head(executor, &conversation, &response).await?;
                }
                Some(fs::CntReqType::Mmap) => {
                    let response = svr_response(fs::Errors::Success);
                    ipc::send_head(executor, &conversation, &response).await?;
                    ipc::push_descriptor(executor, &conversation, file.memory(), FILE_MEMORY_RIGHTS)
                        .await?;
                }
                _ => {
                    error!("freya: illegal request type for a regular file");
                    ipc::dismiss(executor, &conversation).await?;
                }
            }
        } else {
            error!("freya: illegal request ID {} for a regular file", preamble.id());
            ipc::dismiss(executor, &conversation).await?;
        }
    }

    Ok(())
}

/// Copies `buffer.len()` bytes at `offset` out of a file's memory object.
fn read_file(file: &VfsFile, offset: usize, buffer: &mut [u8]) -> hel::Result<()> {
    const PAGE_SIZE: usize = 0x1000;
    let first = offset & !(PAGE_SIZE - 1);
    let end = (offset + buffer.len()).next_multiple_of(PAGE_SIZE);

    // SAFETY: the mapping is private to this function and read-only.
    let mapping: Mapping<u8> =
        unsafe { Mapping::new(file.memory(), None, first, end - first, MappingFlags::READ)? };
    // SAFETY: the mapping covers [first, end), which contains the range read.
    unsafe {
        let source = mapping.as_ptr().expect("freya: empty file mapping").as_ptr();
        buffer.copy_from_slice(core::slice::from_raw_parts(
            source.add(offset - first).cast_const(),
            buffer.len(),
        ));
    }
    Ok(())
}

async fn serve_directory(executor: &Executor, lane: &Handle, node: &VfsNode) -> hel::Result<()> {
    let VfsNode::Directory(directory) = node else {
        unreachable!()
    };
    let mut index = 0usize;

    while let Some((conversation, head)) = ipc::accept(executor, lane).await? {
        let Ok(preamble) = bragi::preamble_from_bytes(&head) else {
            warn!("freya: failed to decode a directory request preamble");
            return Ok(());
        };

        if preamble.id() == <fs::ReadEntriesRequest as bragi::Message>::MESSAGE_ID {
            let mut response = fs::ReadEntriesResponse::default();
            // The listing is taken afresh for every entry, as thor's index into
            // its MFS directory was.
            match directory.entries().into_iter().nth(index) {
                Some((name, entry)) => {
                    response.set_error(fs::Errors::Success);
                    response.set_path(name);
                    response.set_file_type(match &*entry {
                        VfsNode::Directory(_) => fs::FileType::DIRECTORY,
                        _ => fs::FileType::REGULAR,
                    });
                    index += 1;
                }
                None => {
                    response.set_error(fs::Errors::EndOfFile);
                    // Required by the encoder; C++ leaves it zero as well.
                    response.set_file_type(fs::FileType::from(0));
                }
            }
            ipc::send_head_tail(executor, &conversation, &response).await?;
        } else {
            error!("freya: illegal request ID {} for a directory", preamble.id());
            ipc::dismiss(executor, &conversation).await?;
        }
    }

    Ok(())
}

async fn serve_urandom(executor: &Executor, lane: &Handle) -> hel::Result<()> {
    while let Some((conversation, head)) = ipc::accept(executor, lane).await? {
        let Ok(preamble) = bragi::preamble_from_bytes(&head) else {
            warn!("freya: failed to decode a urandom request preamble");
            return Ok(());
        };

        if preamble.id() == <fs::ReadRequest as bragi::Message>::MESSAGE_ID {
            let Ok(request) = bragi::head_from_bytes::<fs::ReadRequest>(&head) else {
                warn!("freya: could not parse a urandom read request");
                return Ok(());
            };
            ipc::extract_credentials(executor, &conversation).await?;

            let mut data = vec![0u8; request.size() as usize];
            let mut filled = 0;
            while filled < data.len() {
                filled += hel::get_random_bytes(&mut data[filled..])?;
            }

            let response = svr_response(fs::Errors::Success);
            ipc::send_head(executor, &conversation, &response).await?;
            ipc::send(executor, &conversation, &data).await?;
        } else {
            error!("freya: illegal request ID {} for /dev/urandom", preamble.id());
            ipc::dismiss(executor, &conversation).await?;
        }
    }

    Ok(())
}
