use std::sync::{Arc, Weak};

use managarm::hw::Device;

use crate::{MailboxChannel, MailboxDevice};

pub struct AppleAscMailboxV4 {
    self_ptr: Weak<Self>,
    hw_device: Device,
}

impl AppleAscMailboxV4 {
    pub async fn new(hw_device: Device) -> anyhow::Result<Arc<Self>> {
        Ok(Arc::new_cyclic(|weak| Self {
            self_ptr: weak.clone(),
            hw_device,
        }))
    }
}

impl MailboxDevice for AppleAscMailboxV4 {
    fn translate_channel(&self, _specifier: &[u32]) -> Option<u32> {
        Some(0)
    }

    fn request_channel(&self, channel: u32) -> Option<MailboxChannel> {
        Some(MailboxChannel {
            device: self.self_ptr.upgrade().unwrap(),
            channel,
        })
    }

    fn send(&self, _channel: &MailboxChannel, _buffer: &[u8]) {
        todo!()
    }

    fn receive(&self, _channel: &MailboxChannel, _buffer: &mut [u8]) {
        todo!()
    }
}
