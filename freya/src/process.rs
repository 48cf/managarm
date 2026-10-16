//! Running servers: loading them, giving them their descriptors, and acting
//! on the supercalls they raise.
//!
//! This replaces thor's `executeModule()` and the observe loop of its POSIX
//! emulation (`service.cpp`). A server gets the environment that mlibc's
//! managarm sysdeps expect from a process: an aux vector, a POSIX lane per
//! thread, an mbus lane, a file table with the three standard streams, and
//! answers to exactly the supercalls that thor answered.

use alloc::rc::Rc;
use alloc::sync::Arc;
use alloc::string::{String, ToString};
use alloc::vec::Vec;
use core::cell::{Cell, RefCell};

use hel::executor::Executor;
use hel::submission::{observe_with_executor, write_memory_with_executor};
use hel::thread::{self, Observation, Registers, ThreadFlags};
use hel::{AllocFlags, Handle, Mapping, MappingFlags};
use managarm_core::data::{
    ManagarmProcessData, ManagarmServerData, PosixSupercall, SvrctlSupercall,
};
use managarm_core::protocols::svrctl;

use crate::files::{self, OpenFile};
use crate::vfs::{self, VfsNode};
use crate::{Freya, elf, error, info, posix};

const PAGE_SIZE: usize = 0x1000;
const STACK_SIZE: usize = 0x10000;
/// Where the dynamic linker is loaded, as thor did.
const INTERPRETER_BASE: usize = 0x4000_0000;

const AT_NULL: usize = 0;
const AT_PHDR: usize = 3;
const AT_PHENT: usize = 4;
const AT_PHNUM: usize = 5;
const AT_ENTRY: usize = 9;
const AT_XPIPE: usize = 0x1000;

/// The token that grants access to I/O ports, physical memory and IRQs.
const HARDWARE_ACCESS_TOKEN: &str = "access:thor.hardware";

/// One thread of a server.
pub struct ThreadInfo {
    pub handle: Handle,
    pub tid: u64,
    /// The thread's POSIX lane, as a handle in the server's universe.
    pub posix_handle: hel_sys::HelHandle,
}

/// A running server and the state that its supercalls and POSIX requests
/// refer to.
pub struct Process {
    pub name: String,
    pub executor: Executor,
    pub universe: Handle,
    pub space: Handle,
    pub hierarchy: Handle,

    // Handles in the server's universe, reported by the supercalls.
    mbus_handle: hel_sys::HelHandle,
    control_handle: hel_sys::HelHandle,
    hierarchy_handle: hel_sys::HelHandle,
    hardware_access_handle: hel_sys::HelHandle,

    /// Maps file descriptors to the handles in the server's universe; the
    /// server reads it through its own read-only mapping at `file_table_address`.
    file_table: Mapping<hel_sys::HelHandle>,
    file_table_address: usize,
    pub files: RefCell<Vec<Option<Rc<OpenFile>>>>,

    next_tid: Cell<u64>,
}

/// How many descriptors fit into the file table page.
const MAX_FILES: usize = PAGE_SIZE / size_of::<hel_sys::HelHandle>();

impl Process {
    /// Installs `client` (one end of a lane that serves `file`) as the lowest
    /// free file descriptor.
    fn add_file(&self, file: Rc<OpenFile>, client: &Handle) -> hel::Result<i32> {
        let mut files = self.files.borrow_mut();
        let fd = match files.iter().position(Option::is_none) {
            Some(fd) => fd,
            None if files.len() < MAX_FILES => {
                files.push(None);
                files.len() - 1
            }
            None => return Err(hel::Error::OutOfBounds),
        };

        let handle = client
            .transfer_into(&self.universe, hel_sys::kHelRightInvoke)?
            .into_raw();
        // SAFETY: the table holds MAX_FILES entries and fd is below that; the
        // server only reads the table.
        unsafe {
            self.file_table
                .as_ptr()
                .expect("freya: file table is not mapped")
                .as_ptr()
                .add(fd)
                .write_volatile(handle);
        }
        files[fd] = Some(file);
        Ok(fd as i32)
    }

    /// Opens `file`: starts serving it on a new lane and installs the lane as a
    /// file descriptor.
    pub fn open_file(&self, file: OpenFile) -> hel::Result<i32> {
        let (server, client) = hel::create_stream()?;
        let file = Rc::new(file);
        self.executor
            .spawn(files::serve(self.executor.clone(), server, file.clone()));
        self.add_file(file, &client)
    }

    /// Starts observing `thread` and serving its POSIX lane. Returns its TID.
    fn attach_thread(self: &Rc<Self>, handle: Handle) -> hel::Result<u64> {
        let (server, client) = hel::create_stream()?;
        let tid = self.next_tid.get();
        self.next_tid.set(tid + 1);

        let info = Rc::new(ThreadInfo {
            handle,
            tid,
            posix_handle: client
                .transfer_into(&self.universe, hel_sys::kHelRightInvoke)?
                .into_raw(),
        });

        self.executor
            .spawn(posix::serve(self.clone(), info.clone(), server));
        self.executor.spawn(observe(self.clone(), info));
        Ok(tid)
    }
}

/// Builds the initial stack contents: no arguments, no environment, and the
/// aux vector that tells the dynamic linker where the program is.
fn initial_stack(exec: &elf::ImageInfo, xpipe: Option<hel_sys::HelHandle>) -> Vec<usize> {
    let mut words = alloc::vec![
        0, // argc
        0, // end of arguments
        0, // end of environment
        AT_ENTRY,
        exec.entry,
        AT_PHDR,
        exec.phdr,
        AT_PHENT,
        exec.phent,
        AT_PHNUM,
        exec.phnum,
    ];
    if let Some(xpipe) = xpipe {
        words.extend([AT_XPIPE, xpipe as usize]);
    }
    words.extend([AT_NULL, 0]);
    // The stack pointer must be 16-byte aligned on entry.
    if words.len() % 2 != 0 {
        words.push(0);
    }
    words
}

fn lookup_file(path: &str) -> Result<Arc<VfsNode>, &'static str> {
    match vfs::lookup(path) {
        Some(node) if matches!(&*node, VfsNode::File(_)) => Ok(node),
        Some(_) => Err("not a regular file"),
        None => Err("no such file"),
    }
}

fn as_file(node: &VfsNode) -> &vfs::VfsFile {
    match node {
        VfsNode::File(file) => file,
        _ => unreachable!(),
    }
}

/// Loads the server that `description` names and starts it.
///
/// `control` is the server's end of its control lane. `xpipe`, if given, is
/// passed through the aux vector; only mbus gets one, for its server lane.
pub fn launch(
    freya: &Rc<Freya>,
    description: &svrctl::Description,
    control: &Handle,
    xpipe: Option<&Handle>,
) -> Result<Rc<Process>, &'static str> {
    let mut tag = String::from("server:");
    tag.push_str(description.name());
    let hierarchy = hel::extend_hierarchy(&freya.hierarchy, &tag)
        .map_err(|_| "failed to extend the hierarchy")?;
    let space = hel::create_space().map_err(|_| "failed to create an address space")?;
    let universe = thread::create_universe().map_err(|_| "failed to create a universe")?;

    let exec_node = lookup_file(description.exec())?;
    let exec = elf::load(&hierarchy, &space, 0, as_file(&exec_node))?;
    let interpreter_path = exec
        .interpreter
        .as_deref()
        .ok_or("the server is not dynamically linked")?;
    let interpreter_node = lookup_file(interpreter_path)?;
    let interpreter = elf::load(&hierarchy, &space, INTERPRETER_BASE, as_file(&interpreter_node))?;

    let transfer = |handle: &Handle, rights| {
        handle
            .transfer_into(&universe, rights)
            .map(Handle::into_raw)
            .map_err(|_| "failed to transfer a descriptor to the server")
    };

    // The stack, with the aux vector at its top.
    let xpipe = xpipe
        .map(|lane| transfer(lane, hel_sys::kHelRightInvoke))
        .transpose()?;
    let stack_words = initial_stack(&exec, xpipe);
    let stack_bytes = stack_words.len() * size_of::<usize>();
    let stack = hel::allocate_memory(&hierarchy, STACK_SIZE, AllocFlags::empty(), None)
        .map_err(|_| "failed to allocate the stack")?;
    {
        // SAFETY: the mapping is private to this block.
        let fill: Mapping<usize> = unsafe {
            Mapping::new(&stack, None, 0, STACK_SIZE, MappingFlags::READ | MappingFlags::WRITE)
        }
        .map_err(|_| "failed to map the stack")?;
        // SAFETY: the words fit at the top of the stack.
        unsafe {
            let top = fill.as_ptr().ok_or("failed to map the stack")?.as_ptr().cast::<u8>();
            top.add(STACK_SIZE - stack_bytes)
                .cast::<usize>()
                .copy_from_nonoverlapping(stack_words.as_ptr(), stack_words.len());
        }
    }
    // SAFETY: `space` is the server's address space.
    let stack_base = unsafe {
        hel::map_memory(&stack, &space, None, 0, STACK_SIZE, MappingFlags::READ | MappingFlags::WRITE)
    }
    .map_err(|_| "failed to map the stack into the server")?;

    // The file table: one page that the server can read and freya writes.
    let file_table_memory =
        hel::allocate_memory(&hierarchy, PAGE_SIZE, AllocFlags::empty(), None)
            .map_err(|_| "failed to allocate the file table")?;
    // SAFETY: freya only ever writes whole handles into the table.
    let file_table = unsafe {
        Mapping::new(
            &file_table_memory,
            None,
            0,
            PAGE_SIZE,
            MappingFlags::READ | MappingFlags::WRITE,
        )
    }
    .map_err(|_| "failed to map the file table")?;
    // SAFETY: `space` is the server's address space.
    let file_table_address = unsafe {
        hel::map_memory(&file_table_memory, &space, None, 0, PAGE_SIZE, MappingFlags::READ)
    }
    .map_err(|_| "failed to map the file table into the server")?;

    let has_hardware_access = description
        .owned_tokens()
        .iter()
        .any(|token| token.identifier() == HARDWARE_ACCESS_TOKEN);
    let hardware_access_handle = if has_hardware_access {
        transfer(&freya.hardware_access, hel_sys::kHelRightInvoke)?
    } else {
        hel_sys::kHelNullHandle as hel_sys::HelHandle
    };

    let process = Rc::new(Process {
        name: description.exec().to_string(),
        executor: freya.executor.clone(),
        mbus_handle: transfer(&freya.mbus_client, hel_sys::kHelRightInvoke)?,
        control_handle: transfer(control, hel_sys::kHelRightInvoke)?,
        hierarchy_handle: transfer(
            &hierarchy,
            hel_sys::kHelRightDerive | hel_sys::kHelRightProvision,
        )?,
        hardware_access_handle,
        universe,
        space,
        hierarchy,
        file_table,
        file_table_address,
        files: RefCell::new(Vec::new()),
        next_tid: Cell::new(1),
    });

    // The standard streams all share one lane that goes to the kernel log.
    let (stdio_server, stdio_client) =
        hel::create_stream().map_err(|_| "failed to create the stdio lane")?;
    let stdio = Rc::new(OpenFile::Stdio);
    freya
        .executor
        .spawn(files::serve(freya.executor.clone(), stdio_server, stdio.clone()));
    for _ in 0..3 {
        process
            .add_file(stdio.clone(), &stdio_client)
            .map_err(|_| "failed to install the standard streams")?;
    }

    let main_thread = thread::create_thread(
        &process.universe,
        &process.space,
        interpreter.entry,
        stack_base + STACK_SIZE - stack_bytes,
        ThreadFlags::STOPPED,
    )
    .map_err(|_| "failed to create the main thread")?;
    // Resume before observing: a thread that was created stopped reports that
    // stop to its first observer.
    thread::resume(&main_thread).map_err(|_| "failed to start the main thread")?;
    process
        .attach_thread(main_thread)
        .map_err(|_| "failed to attach the main thread")?;

    Ok(process)
}

/// Resumes a thread after its supercall has been answered.
fn complete(thread: &ThreadInfo, registers: &mut Registers, error: hel_sys::HelError, outputs: &[usize]) {
    registers.set_result(error, outputs);
    registers
        .store(&thread.handle)
        .expect("freya: failed to store the registers of a server thread");
    thread::resume(&thread.handle).expect("freya: failed to resume a server thread");
}

const ERR_NONE: hel_sys::HelError = hel_sys::kHelErrNone as hel_sys::HelError;

/// Acts on everything that stops `thread` until it exits or dies.
async fn observe(process: Rc<Process>, thread: Rc<ThreadInfo>) {
    let executor = process.executor.clone();

    loop {
        let observation = match observe_with_executor(executor.clone(), &thread.handle).await {
            Ok(observation) => observation,
            Err(hel::Error::ThreadTerminated) => break,
            Err(error) => {
                error!("freya: failed to observe a thread of {}: {error}", process.name);
                break;
            }
        };

        let call = match observation {
            Observation::SuperCall(call) => call,
            Observation::Null => {
                // The stop that a thread created stopped starts with.
                let _ = thread::resume(&thread.handle);
                continue;
            }
            Observation::Panic => {
                // Stop observing; the thread stays stopped. gdbserver is not
                // available for servers that freya runs.
                error!("freya: Panic in server {}", process.name);
                break;
            }
            Observation::PageFault
            | Observation::GeneralFault
            | Observation::IllegalInstruction
            | Observation::DivByZero
            | Observation::Breakpoint => {
                error!("freya: Fault in server {} ({observation:?})", process.name);
                break;
            }
            other => {
                error!("freya: unexpected observation {other:?} in server {}", process.name);
                break;
            }
        };

        let mut registers = match Registers::load(&thread.handle) {
            Ok(registers) => registers,
            Err(error) => {
                error!("freya: failed to load the registers of {}: {error}", process.name);
                break;
            }
        };

        match call {
            c if c == PosixSupercall::AnonAllocate as u32 => {
                let size = registers.arg(0);
                let result = hel::copy_on_write(
                    &process.hierarchy,
                    None,
                    vfs::zero_memory(),
                    0,
                    size,
                )
                .and_then(|memory| unsafe {
                    // SAFETY: `space` is the server's address space.
                    hel::map_memory(
                        &memory,
                        &process.space,
                        None,
                        0,
                        size,
                        MappingFlags::READ | MappingFlags::WRITE,
                    )
                });
                match result {
                    Ok(address) => complete(&thread, &mut registers, ERR_NONE, &[address]),
                    Err(error) => complete(&thread, &mut registers, error.into(), &[0]),
                }
            }
            c if c == PosixSupercall::AnonDeallocate as u32 => {
                let (address, size) = (registers.arg(0), registers.arg(1));
                // SAFETY: `space` is the server's address space.
                let error = match unsafe { hel::unmap_memory(&process.space, address, size) } {
                    Ok(()) => ERR_NONE,
                    Err(_) => hel_sys::kHelErrIllegalArgs as hel_sys::HelError,
                };
                complete(&thread, &mut registers, error, &[0]);
            }
            c if c == PosixSupercall::GetProcessData as u32 => {
                let data = ManagarmProcessData {
                    posix_lane: thread.posix_handle,
                    mbus_lane: process.mbus_handle,
                    thread_page: 0,
                    file_table: process.file_table_address,
                    clock_tracker_page: 0,
                    hierarchy_handle: process.hierarchy_handle,
                };
                let error = write_out(&process, &registers, data.as_bytes()).await;
                complete(&thread, &mut registers, error, &[]);
            }
            c if c == SvrctlSupercall::GetServerData as u32 => {
                let data = ManagarmServerData {
                    hardware_access: process.hardware_access_handle,
                    control_lane: process.control_handle,
                };
                let error = write_out(&process, &registers, data.as_bytes()).await;
                complete(&thread, &mut registers, error, &[]);
            }
            c if c == PosixSupercall::SigMask as u32 => {
                complete(&thread, &mut registers, ERR_NONE, &[0]);
            }
            c if c == PosixSupercall::GetTid as u32 => {
                complete(&thread, &mut registers, ERR_NONE, &[thread.tid as usize]);
            }
            c if c == PosixSupercall::Clone as u32 => {
                match clone_thread(&process, &registers) {
                    // The second output is the POSIX error; zero is SUCCESS.
                    Ok(tid) => complete(&thread, &mut registers, ERR_NONE, &[0, tid as usize]),
                    Err(error) => {
                        error!("freya: clone in {} failed: {error}", process.name);
                        complete(&thread, &mut registers, error.into(), &[0, 0]);
                    }
                }
            }
            c if c == PosixSupercall::Exit as u32 => break,
            c if c == PosixSupercall::SigKill as u32 => {
                error!("freya: Signal sent by server {}", process.name);
                break;
            }
            other => {
                error!("freya: unexpected supercall {other} in server {}", process.name);
                break;
            }
        }
    }
}

/// Writes `data` to the address in the first supercall argument, truncated
/// to the size in the second, and returns the error to report.
async fn write_out(process: &Process, registers: &Registers, data: &[u8]) -> hel_sys::HelError {
    let (address, size) = (registers.arg(0), registers.arg(1));
    let length = size.min(data.len());
    match write_memory_with_executor(
        process.executor.clone(),
        &process.space,
        address,
        &data[..length],
    )
    .await
    {
        Ok(()) => ERR_NONE,
        Err(_) => hel_sys::kHelErrFault as hel_sys::HelError,
    }
}

/// Starts a new thread of `process` at the instruction and stack pointer in
/// the supercall arguments and returns its TID.
fn clone_thread(process: &Rc<Process>, registers: &Registers) -> hel::Result<u64> {
    let (ip, sp) = (registers.arg(0), registers.arg(1));
    let new_thread = thread::create_thread(
        &process.universe,
        &process.space,
        ip,
        sp,
        ThreadFlags::STOPPED,
    )?;

    // gp points to the executable's small data area and is only ever computed
    // by crt0, hence the new thread has to inherit it from the calling thread.
    #[cfg(target_arch = "riscv64")]
    {
        let mut new_registers = Registers::load(&new_thread)?;
        new_registers.0[hel_sys::kHelRegGp as usize] = registers.0[hel_sys::kHelRegGp as usize];
        new_registers.store(&new_thread)?;
    }

    thread::resume(&new_thread)?;
    process.attach_thread(new_thread)
}

/// The servers that were started, by name, with the client end of their
/// control lane. svrctl hands these out to whoever runs a server again.
pub type ServerRegistry = RefCell<alloc::collections::BTreeMap<String, Handle>>;

/// Starts the server that `description` names unless one of that name runs
/// already, and returns the client end of its control lane.
pub fn run_server(
    freya: &Rc<Freya>,
    description: &svrctl::Description,
    xpipe: Option<&Handle>,
) -> Result<Handle, &'static str> {
    if let Some(control) = freya.servers.borrow().get(description.name()) {
        info!("freya: server {} is already running", description.name());
        return control
            .clone_handle()
            .map_err(|_| "failed to duplicate a control lane");
    }

    info!("freya: launching server {}", description.name());
    let (server_end, client_end) =
        hel::create_stream().map_err(|_| "failed to create a control lane")?;
    launch(freya, description, &server_end, xpipe)?;

    let returned = client_end
        .clone_handle()
        .map_err(|_| "failed to duplicate a control lane")?;
    freya
        .servers
        .borrow_mut()
        .insert(description.name().to_string(), client_end);
    Ok(returned)
}
