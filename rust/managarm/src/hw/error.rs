use super::bindings::Errors;

use thiserror::Error;

#[derive(Debug, Error)]
pub enum Error {
    #[error("Out of bounds")]
    OutOfBounds,
    #[error("Illegal arguments")]
    IllegalArguments,
    #[error("Resource exhaustion")]
    ResourceExhaustion,
    #[error("Device error")]
    DeviceError,
    #[error("Property not found")]
    PropertyNotFound,
    #[error("Hel error: {0:?}")]
    HelError(#[from] hel::Error),
    #[error("IO error: {0:?}")]
    IoError(#[from] std::io::Error),
}

impl From<Errors> for Error {
    fn from(value: Errors) -> Self {
        match value {
            Errors::Success => unreachable!(),
            Errors::OutOfBounds => Error::OutOfBounds,
            Errors::IllegalArguments => Error::IllegalArguments,
            Errors::ResourceExhaustion => Error::ResourceExhaustion,
            Errors::DeviceError => Error::DeviceError,
            Errors::PropertyNotFound => Error::PropertyNotFound,
        }
    }
}
