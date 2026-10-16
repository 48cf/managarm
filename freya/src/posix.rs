//! The POSIX lane of the servers that freya runs.
//!
//! This answers the same small set of requests that thor's POSIX emulation
//! did; everything else is dismissed. Servers only need enough of POSIX to get
//! through mlibc's and Rust's startup and to open and map initrd files.

use alloc::rc::Rc;

use hel::submission::protect_memory_with_executor;
use hel::{Handle, MappingFlags};
use managarm_core::protocols::posix;

use crate::files::OpenFile;
use crate::process::{Process, ThreadInfo};
use crate::vfs::{self, VfsNode};
use crate::{error, info, ipc, warn};

const PROT_READ: u32 = 0x1;
const PROT_WRITE: u32 = 0x2;
const PROT_EXEC: u32 = 0x4;

const MAP_PRIVATE: u32 = 0x02;
const MAP_FIXED: u32 = 0x10;
const MAP_ANONYMOUS: u32 = 0x20;

/// `AT_FDCWD`; there is no other directory to resolve paths against.
const AT_FDCWD: i32 = -100;
const AT_EMPTY_PATH: i32 = 0x1000;

fn protection(mode: u32) -> MappingFlags {
    let mut flags = MappingFlags::empty();
    if mode & PROT_READ != 0 {
        flags |= MappingFlags::READ;
    }
    if mode & PROT_WRITE != 0 {
        flags |= MappingFlags::WRITE;
    }
    if mode & PROT_EXEC != 0 {
        flags |= MappingFlags::EXECUTE;
    }
    flags
}

/// Serves the POSIX lane of one thread of `process`.
pub async fn serve(process: Rc<Process>, thread: Rc<ThreadInfo>, lane: Handle) {
    if let Err(error) = serve_requests(&process, &thread, &lane).await {
        warn!("freya: stopped serving the POSIX lane of {}: {error}", process.name);
    }
}

async fn serve_requests(process: &Rc<Process>, thread: &ThreadInfo, lane: &Handle) -> hel::Result<()> {
    let executor = &process.executor;

    while let Some((conversation, head)) = ipc::accept(executor, lane).await? {
        let Ok(preamble) = bragi::preamble_from_bytes(&head) else {
            warn!("freya: failed to decode a POSIX request preamble from {}", process.name);
            return Ok(());
        };
        let id = preamble.id();

        macro_rules! parse_head {
            ($ty:ty) => {
                match bragi::head_from_bytes::<$ty>(&head) {
                    Ok(request) => request,
                    Err(_) => {
                        warn!("freya: could not parse a POSIX request from {}", process.name);
                        return Ok(());
                    }
                }
            };
        }
        macro_rules! parse_head_tail {
            ($ty:ty) => {{
                let tail = ipc::recv_inline(executor, &conversation).await?;
                match bragi::head_tail_from_bytes::<$ty>(&head, &tail) {
                    Ok(request) => request,
                    Err(_) => {
                        warn!("freya: could not parse a POSIX request from {}", process.name);
                        return Ok(());
                    }
                }
            }};
        }
        macro_rules! is {
            ($ty:ty) => {
                id == <$ty as bragi::Message>::MESSAGE_ID
            };
        }

        if is!(posix::SigactionRequest) {
            let _request = parse_head!(posix::SigactionRequest);
            let mut response = posix::SigactionResponse::default();
            response.set_error(posix::Errors::IllegalRequest);
            ipc::send_head(executor, &conversation, &response).await?;
        } else if is!(posix::VmProtectRequest) {
            let request = parse_head!(posix::VmProtectRequest);
            let mut response = posix::VmProtectResponse::default();
            if request.mode() & !(PROT_READ | PROT_WRITE | PROT_EXEC) != 0 {
                response.set_error(posix::Errors::IllegalArguments);
            } else {
                let result = protect_memory_with_executor(
                    executor.clone(),
                    &process.space,
                    request.address() as usize,
                    request.size() as usize,
                    protection(request.mode()),
                )
                .await;
                response.set_error(match result {
                    Ok(()) => posix::Errors::Success,
                    Err(_) => posix::Errors::IllegalArguments,
                });
            }
            ipc::send_head(executor, &conversation, &response).await?;
        } else if is!(posix::OpenAtRequest) {
            let request = parse_head_tail!(posix::OpenAtRequest);
            let mut response = posix::OpenAtResponse::default();
            if request.fd() != AT_FDCWD {
                info!("freya: OpenAt from {} does not support dirfds", process.name);
                response.set_error(posix::Errors::IllegalArguments);
            } else {
                let file = vfs::lookup(request.path()).map(|node| match &*node {
                    VfsNode::Directory(_) => OpenFile::Directory(node),
                    VfsNode::File(_) => OpenFile::Regular(node),
                    VfsNode::Urandom { .. } => OpenFile::Urandom(node),
                });
                match file.map(|file| process.open_file(file)) {
                    None => response.set_error(posix::Errors::FileNotFound),
                    Some(Ok(fd)) => {
                        response.set_error(posix::Errors::Success);
                        response.set_fd(fd);
                    }
                    Some(Err(error)) => {
                        error!("freya: failed to open {} for {}: {error}", request.path(), process.name);
                        response.set_error(posix::Errors::NoMemory);
                    }
                }
            }
            ipc::send_head(executor, &conversation, &response).await?;
        } else if is!(posix::FstatAtRequest) {
            let request = parse_head_tail!(posix::FstatAtRequest);
            let mut response = posix::FstatAtResponse::default();

            let stated = if request.flags() & AT_EMPTY_PATH != 0 && request.path().is_empty() {
                // fstat(): rtld uses the inode to tell loaded libraries apart.
                let files = process.files.borrow();
                match usize::try_from(request.fd()).ok().and_then(|fd| files.get(fd)?.clone()) {
                    Some(file) => match file.node() {
                        Some(node) => stat_node(&mut response, node),
                        None => {
                            // The standard streams go to the kernel log.
                            response.set_file_type(posix::FileType::FT_CHAR_DEVICE);
                            true
                        }
                    },
                    None => false,
                }
            } else if request.fd() == AT_FDCWD {
                match vfs::lookup(request.path()) {
                    Some(node) => stat_node(&mut response, &node),
                    None => false,
                }
            } else {
                false
            };

            if stated {
                response.set_error(posix::Errors::Success);
            } else {
                info!(
                    "freya: cannot stat '{}' (fd {}) for {}",
                    request.path(),
                    request.fd(),
                    process.name
                );
                response.set_error(posix::Errors::FileNotFound);
                // Required by the encoder; C++ leaves it zero as well.
                response.set_file_type(posix::FileType::FT_UNKNOWN);
            }
            ipc::send_head(executor, &conversation, &response).await?;
        } else if is!(posix::IsTtyRequest) {
            let request = parse_head!(posix::IsTtyRequest);
            let mut response = posix::IsTtyResponse::default();
            let files = process.files.borrow();
            match usize::try_from(request.fd()).ok().and_then(|fd| files.get(fd)?.as_ref()) {
                Some(file) => {
                    response.set_error(posix::Errors::Success);
                    response.set_mode(file.is_terminal() as i32);
                }
                None => response.set_error(posix::Errors::NoSuchFd),
            }
            drop(files);
            ipc::send_head(executor, &conversation, &response).await?;
        } else if is!(posix::CloseRequest) {
            // Like thor, keep every file open: the file table is never
            // compacted, and servers open only a handful of files.
            let _request = parse_head!(posix::CloseRequest);
            let mut response = posix::CloseResponse::default();
            response.set_error(posix::Errors::Success);
            ipc::send_head(executor, &conversation, &response).await?;
        } else if is!(posix::VmMapRequest) {
            let request = parse_head!(posix::VmMapRequest);
            let mut response = posix::VmMapResponse::default();
            match vm_map(process, &request) {
                Ok(address) => {
                    response.set_error(posix::Errors::Success);
                    response.set_offset(address as u64);
                }
                Err(error) => response.set_error(error),
            }
            ipc::send_head(executor, &conversation, &response).await?;
        } else if is!(posix::GetPidRequest) {
            let _request = parse_head!(posix::GetPidRequest);
            let mut response = posix::GetPidResponse::default();
            response.set_error(posix::Errors::Success);
            response.set_pid(thread.tid as i64);
            ipc::send_head(executor, &conversation, &response).await?;
        } else if is!(posix::EpollCallRequest) {
            // Rust calls poll() during program startup.
            let _tail = ipc::recv_inline(executor, &conversation).await?;
            let mut response = posix::EpollCallResponse::default();
            response.set_error(posix::Errors::IllegalArguments);
            ipc::send_head_tail(executor, &conversation, &response).await?;
        } else if is!(posix::FdGetFlagsRequest) {
            let _request = parse_head!(posix::FdGetFlagsRequest);
            let mut response = posix::FdGetFlagsResponse::default();
            response.set_error(posix::Errors::IllegalRequest);
            ipc::send_head(executor, &conversation, &response).await?;
        } else {
            info!("freya: illegal POSIX request type {id} from {}", process.name);
            ipc::dismiss(executor, &conversation).await?;
        }
    }

    Ok(())
}

/// Describes `node` in `response`. Always succeeds; the result says so to
/// keep the call sites uniform.
fn stat_node(response: &mut posix::FstatAtResponse, node: &VfsNode) -> bool {
    let (file_type, size) = match node {
        VfsNode::File(file) => (posix::FileType::FT_REGULAR, file.len() as u64),
        VfsNode::Directory(_) => (posix::FileType::FT_DIRECTORY, 0),
        // thor kept /dev/urandom as an empty regular file.
        VfsNode::Urandom { .. } => (posix::FileType::FT_REGULAR, 0),
    };
    response.set_file_type(file_type);
    response.set_file_size(size);
    response.set_fs_inode(node.inode());
    response.set_num_links(1);
    true
}

/// Maps anonymous memory or an initrd file into the server, privately.
fn vm_map(process: &Process, request: &posix::VmMapRequest) -> Result<usize, posix::Errors> {
    let size = request.size() as usize;
    if size == 0 || request.flags() & !(MAP_ANONYMOUS | MAP_FIXED | MAP_PRIVATE) != 0 {
        return Err(posix::Errors::IllegalArguments);
    }
    // Shared mappings were never implemented for servers.
    if request.flags() & MAP_PRIVATE == 0 {
        return Err(posix::Errors::IllegalArguments);
    }

    let mut flags = protection(request.mode());
    let address = if request.flags() & MAP_FIXED != 0 {
        flags |= MappingFlags::FIXED;
        Some(request.address_hint() as usize)
    } else {
        None
    };

    let files = process.files.borrow();
    let source = if request.flags() & MAP_ANONYMOUS != 0 {
        vfs::zero_memory()
    } else {
        usize::try_from(request.fd())
            .ok()
            .and_then(|fd| files.get(fd)?.as_ref()?.memory())
            .ok_or(posix::Errors::BadFd)?
    };
    let offset = usize::try_from(request.rel_offset()).map_err(|_| posix::Errors::IllegalArguments)?;

    let memory = hel::copy_on_write(&process.hierarchy, None, source, offset, size)
        .map_err(|_| posix::Errors::IllegalArguments)?;
    // SAFETY: `space` is the server's address space.
    unsafe { hel::map_memory(&memory, &process.space, address, 0, size, flags) }
        .map_err(|_| posix::Errors::IllegalArguments)
}
