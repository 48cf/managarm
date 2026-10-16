//! Helpers for the request/response conversations that freya serves.
//!
//! Every server freya runs here works the same way: accept a conversation on a
//! lane, receive the request head (and maybe a tail or a data buffer), and
//! answer with one or more buffers or descriptors.

use alloc::vec;
use alloc::vec::Vec;

use hel::executor::Executor;
use hel::submission::submit_async_with_executor;
use hel::{
    Accept, Dismiss, Error, ExtractCredentials, Handle, PushDescriptor, ReceiveBuffer,
    ReceiveInline, Result, SendBuffer,
};

/// Accepts the next conversation on `lane` and receives its first buffer.
/// Returns `None` once the other side has closed the lane.
pub async fn accept(executor: &Executor, lane: &Handle) -> Result<Option<(Handle, Vec<u8>)>> {
    let (conversation, head) =
        submit_async_with_executor(executor.clone(), lane, Accept::new(ReceiveInline)).await?;
    let conversation = match conversation {
        Ok(Some(conversation)) => conversation,
        Ok(None) => return Err(Error::IllegalState),
        Err(Error::EndOfLane) => return Ok(None),
        Err(error) => return Err(error),
    };
    Ok(Some((conversation, head?)))
}

/// Receives a buffer that fits into a queue element, such as a request tail.
pub async fn recv_inline(executor: &Executor, conversation: &Handle) -> Result<Vec<u8>> {
    submit_async_with_executor(executor.clone(), conversation, ReceiveInline).await?
}

/// Receives a buffer of at most `max` bytes and returns what was sent.
pub async fn recv_buffer(
    executor: &Executor,
    conversation: &Handle,
    max: usize,
) -> Result<Vec<u8>> {
    let mut buffer = vec![0; max];
    let length = submit_async_with_executor(
        executor.clone(),
        conversation,
        ReceiveBuffer::new(&mut buffer),
    )
    .await??;
    buffer.truncate(length);
    Ok(buffer)
}

/// Takes the credentials that the client imbued into the conversation.
pub async fn extract_credentials(executor: &Executor, conversation: &Handle) -> Result<[u8; 16]> {
    submit_async_with_executor(executor.clone(), conversation, ExtractCredentials).await?
}

pub async fn send(executor: &Executor, conversation: &Handle, data: &[u8]) -> Result<()> {
    submit_async_with_executor(executor.clone(), conversation, SendBuffer::new(data)).await?
}

/// Sends the head of `message`, for messages without a tail.
pub async fn send_head<M: bragi::Message>(
    executor: &Executor,
    conversation: &Handle,
    message: &M,
) -> Result<()> {
    let head = bragi::head_to_bytes(message).map_err(|_| Error::IllegalArgs)?;
    send(executor, conversation, &head).await
}

/// Sends the head and the tail of `message` as two buffers.
pub async fn send_head_tail<M: bragi::Message>(
    executor: &Executor,
    conversation: &Handle,
    message: &M,
) -> Result<()> {
    let (head, tail) = bragi::head_tail_to_bytes(message).map_err(|_| Error::IllegalArgs)?;
    send(executor, conversation, &head).await?;
    send(executor, conversation, &tail).await
}

pub async fn push_descriptor(
    executor: &Executor,
    conversation: &Handle,
    descriptor: &Handle,
    rights: hel_sys::HelRights,
) -> Result<()> {
    submit_async_with_executor(
        executor.clone(),
        conversation,
        PushDescriptor::new(descriptor, rights),
    )
    .await?
}

/// Refuses a conversation whose request is not understood.
pub async fn dismiss(executor: &Executor, conversation: &Handle) -> Result<()> {
    submit_async_with_executor(executor.clone(), conversation, Dismiss).await?
}
