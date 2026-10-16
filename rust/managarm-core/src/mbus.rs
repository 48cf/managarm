//! Publishing objects on mbus without `std`.
//!
//! An object is created with [`create_object`]. mbus then asks its owner for a
//! lane whenever a client wants to talk to the object, and the owner answers
//! each request with a fresh lane that only that client holds. This is the same
//! model that thor's `KernelBusObject` follows.

use alloc::collections::BTreeMap;
use alloc::string::{String, ToString};
use alloc::vec::Vec;
use core::fmt;

use hel::executor::Executor;
use hel::submission::submit_async_with_executor;
use hel::{Handle, Offer, PullDescriptor, PushDescriptor, ReceiveInline, SendBuffer};

use crate::protocols::mbus as bindings;

/// A property value of an mbus object.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Item {
    String(String),
    Array(Vec<Item>),
}

impl Item {
    fn encode(&self) -> bindings::AnyItem {
        let mut item = bindings::AnyItem::default();

        match self {
            Self::String(string) => {
                item.set_type(bindings::ItemType::String);
                item.set_string_item(string.clone());
            }
            Self::Array(array) => {
                item.set_type(bindings::ItemType::Array);
                item.set_items(array.iter().map(Self::encode).collect());
            }
        }

        item
    }
}

pub type Properties = BTreeMap<String, Item>;

#[derive(Debug)]
pub enum Error {
    /// A hel operation failed.
    Hel(hel::Error),
    /// A message could not be encoded or decoded.
    Bragi(bragi::io::Error),
    /// mbus answered with an error.
    Mbus(bindings::Error),
    /// mbus answered without the descriptor that the protocol requires.
    MissingDescriptor,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Hel(error) => write!(f, "hel error: {error}"),
            Self::Bragi(error) => write!(f, "bragi error: {error}"),
            Self::Mbus(error) => write!(f, "mbus error: {error:?}"),
            Self::MissingDescriptor => f.write_str("mbus did not send the expected descriptor"),
        }
    }
}

impl core::error::Error for Error {}

impl From<hel::Error> for Error {
    fn from(error: hel::Error) -> Self {
        Self::Hel(error)
    }
}

impl From<bragi::io::Error> for Error {
    fn from(error: bragi::io::Error) -> Self {
        Self::Bragi(error)
    }
}

pub type Result<T> = core::result::Result<T, Error>;

fn check(error: bindings::Error) -> Result<()> {
    if error == bindings::Error::Success {
        Ok(())
    } else {
        Err(Error::Mbus(error))
    }
}

/// An mbus object that this process created and serves.
pub struct Object {
    id: i64,
    mgmt_lane: Handle,
}

/// Creates an mbus object called `name` with the given properties.
///
/// `mbus_lane` is the client lane to mbus, which every process shares.
pub async fn create_object(
    executor: &Executor,
    mbus_lane: &Handle,
    name: &str,
    properties: &Properties,
) -> Result<Object> {
    let request = bindings::CreateObjectRequest::new(
        name.to_string(),
        properties
            .iter()
            .map(|(key, value)| bindings::Property::new(key.clone(), value.encode()))
            .collect(),
    );

    let (head, tail) = bragi::head_tail_to_bytes(&request)?;
    let (offer, (send_head, send_tail, recv, pull)) = submit_async_with_executor(
        executor.clone(),
        mbus_lane,
        Offer::new((
            SendBuffer::new(&head),
            SendBuffer::new(&tail),
            ReceiveInline,
            PullDescriptor::new(hel_sys::kHelRightInvoke | hel_sys::kHelRightManage),
        )),
    )
    .await?;
    offer?;
    send_head?;
    send_tail?;

    let response: bindings::CreateObjectResponse = bragi::head_from_bytes(&recv?)?;
    check(response.error())?;

    Ok(Object {
        id: response.id(),
        mgmt_lane: pull?.ok_or(Error::MissingDescriptor)?,
    })
}

impl Object {
    pub fn id(&self) -> i64 {
        self.id
    }

    /// Waits until mbus asks for a lane to this object on behalf of a client,
    /// and hands it `lane`. The caller serves requests on the other end.
    pub async fn serve_remote_lane(&self, executor: &Executor, lane: &Handle) -> Result<()> {
        let head = bragi::head_to_bytes(&bindings::ServeRemoteLaneRequest::new())?;
        let (offer, (send_head, push, recv)) = submit_async_with_executor(
            executor.clone(),
            &self.mgmt_lane,
            Offer::new((
                SendBuffer::new(&head),
                PushDescriptor::new(lane, hel_sys::kHelRightInvoke | hel_sys::kHelRightManage),
                ReceiveInline,
            )),
        )
        .await?;
        offer?;
        send_head?;
        push?;

        let response: bindings::ServeRemoteLaneResponse = bragi::head_from_bytes(&recv?)?;
        check(response.error())
    }
}
