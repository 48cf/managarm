//! The svrctl mbus object, through which userspace runs further servers.
//!
//! `runsvr` (and the udev rules that call it) uploads the files a server needs
//! and then asks for the server to be run. This was thor's `SvrctlBusObject`.

use alloc::collections::BTreeMap;
use alloc::rc::Rc;
use alloc::string::ToString;

use hel::{AllocFlags, Handle, Mapping, MappingFlags};
use managarm_core::mbus::{self, Item};
use managarm_core::protocols::svrctl;

use crate::vfs::{self, UploadOutcome};
use crate::{Freya, error, ipc, process, warn};

/// The largest file that can be uploaded. The receive buffer is on-demand
/// memory, so only the pages that an upload actually fills are backed.
const MAX_UPLOAD_SIZE: usize = 256 << 20;

/// Publishes the svrctl object and serves it forever.
pub async fn run(freya: Rc<Freya>) {
    let mut properties = BTreeMap::new();
    properties.insert("class".to_string(), Item::String("svrctl".to_string()));

    let object = match mbus::create_object(&freya.executor, &freya.mbus_client, "svrctl", &properties)
        .await
    {
        Ok(object) => object,
        Err(error) => {
            error!("freya: failed to create the svrctl mbus object: {error}");
            return;
        }
    };

    // mbus asks for a lane each time a client wants to talk to svrctl; each
    // client gets a lane of its own.
    loop {
        let (server, client) = match hel::create_stream() {
            Ok(lanes) => lanes,
            Err(error) => {
                error!("freya: failed to create a svrctl lane: {error}");
                return;
            }
        };
        freya.executor.spawn(serve(freya.clone(), server));
        if let Err(error) = object.serve_remote_lane(&freya.executor, &client).await {
            warn!("freya: failed to hand a svrctl lane to mbus: {error}");
        }
    }
}

async fn serve(freya: Rc<Freya>, lane: Handle) {
    if let Err(error) = serve_requests(&freya, &lane).await {
        warn!("freya: stopped serving a svrctl client: {error}");
    }
}

async fn serve_requests(freya: &Rc<Freya>, lane: &Handle) -> hel::Result<()> {
    let executor = &freya.executor;

    while let Some((conversation, head)) = ipc::accept(executor, lane).await? {
        let Ok(preamble) = bragi::preamble_from_bytes(&head) else {
            warn!("freya: failed to decode a svrctl request preamble");
            return Ok(());
        };

        if preamble.id() == <svrctl::FileUploadRequest as bragi::Message>::MESSAGE_ID {
            let Ok(request) = bragi::head_from_bytes::<svrctl::FileUploadRequest>(&head) else {
                warn!("freya: could not parse a svrctl upload request");
                return Ok(());
            };

            let error = if request.with_data() != 0 {
                upload(freya, &conversation, request.name()).await?
            } else if vfs::lookup(request.name()).is_some() {
                svrctl::Errors::Success
            } else {
                svrctl::Errors::DataRequired
            };

            let mut response = svrctl::FileUploadResponse::default();
            response.set_error(error);
            ipc::send_head(executor, &conversation, &response).await?;
        } else if preamble.id() == <svrctl::RunServerRequest as bragi::Message>::MESSAGE_ID {
            let Ok(request) = bragi::head_from_bytes::<svrctl::RunServerRequest>(&head) else {
                warn!("freya: could not parse a svrctl run request");
                return Ok(());
            };

            let mut response = svrctl::RunServerResponse::default();
            match process::run_server(freya, request.description(), None) {
                Ok(control) => {
                    response.set_error(svrctl::Errors::Success);
                    ipc::send_head(executor, &conversation, &response).await?;
                    ipc::push_descriptor(executor, &conversation, &control, hel_sys::kHelRightInvoke)
                        .await?;
                }
                Err(reason) => {
                    error!(
                        "freya: failed to run server {}: {reason}",
                        request.description().name()
                    );
                    response.set_error(svrctl::Errors::IllegalRequest);
                    ipc::send_head(executor, &conversation, &response).await?;
                }
            }
        } else {
            warn!("freya: illegal message ID {} for svrctl", preamble.id());
            ipc::dismiss(executor, &conversation).await?;
        }
    }

    Ok(())
}

/// Receives the data of an upload and adds it to the file tree.
async fn upload(
    freya: &Freya,
    conversation: &Handle,
    path: &str,
) -> hel::Result<svrctl::Errors> {
    let memory = hel::allocate_memory(&freya.hierarchy, MAX_UPLOAD_SIZE, AllocFlags::ON_DEMAND, None)?;
    // SAFETY: the mapping is private to this function.
    let mapping: Mapping<u8> = unsafe {
        Mapping::new(
            &memory,
            None,
            0,
            MAX_UPLOAD_SIZE,
            MappingFlags::READ | MappingFlags::WRITE,
        )?
    };
    // SAFETY: the mapping is MAX_UPLOAD_SIZE bytes long and only used here.
    let buffer = unsafe {
        core::slice::from_raw_parts_mut(
            mapping.as_ptr().ok_or(hel::Error::NoMemory)?.as_ptr(),
            MAX_UPLOAD_SIZE,
        )
    };
    let length = hel::submission::submit_async_with_executor(
        freya.executor.clone(),
        conversation,
        hel::ReceiveBuffer::new(buffer),
    )
    .await??;

    Ok(match vfs::upload(freya.hierarchy.handle(), path, &buffer[..length]) {
        UploadOutcome::Created => svrctl::Errors::Success,
        UploadOutcome::Exists { size } if size == length => svrctl::Errors::Success,
        // Like thor, only the size is compared: comparing the contents would
        // mean mapping the existing file.
        UploadOutcome::Exists { .. } => svrctl::Errors::DataMismatch,
        UploadOutcome::Invalid => {
            warn!("freya: rejected an upload to invalid path {path}");
            svrctl::Errors::IllegalRequest
        }
    })
}
