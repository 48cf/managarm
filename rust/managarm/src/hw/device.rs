use hel::Handle;

use super::{Error, Result, bindings, pci::PciInfo};

pub struct Device {
    handle: Handle,
}

/*

struct DtRegister {
    uintptr_t address;
    size_t length;
    ptrdiff_t offset;
};

struct DtInfo {
    std::vector<DtRegister> regs;
    uint32_t numIrqs;
};

struct DtProperty {
private:
    std::vector<uint8_t> data_;
};

async::result<DtInfo> getDtInfo();
async::result<std::string> getDtPath();
async::result<std::optional<DtProperty>> getDtProperty(std::string_view name);
async::result<std::vector<std::pair<std::string, DtProperty>>> getDtProperties();
async::result<helix::UniqueDescriptor> accessDtRegister(uint32_t index);
async::result<helix::UniqueDescriptor> installDtIrq(uint32_t index);

*/

pub struct DtProperty {
    data: Vec<u8>,
}

impl DtProperty {
    fn from_bytes(data: &[u8]) -> Self {
        Self {
            data: data.to_vec(),
        }
    }

    pub fn data(&self) -> &[u8] {
        &self.data
    }

    pub fn get_u32(&self, offset: usize) -> Option<u32> {
        if offset + size_of::<u32>() < self.data.len() {
            let mut bytes = [0; size_of::<u32>()];
            bytes.copy_from_slice(&self.data[offset..offset + size_of::<u32>()]);
            Some(u32::from_be_bytes(bytes))
        } else {
            None
        }
    }

    pub fn get_u64(&self, offset: usize) -> Option<u64> {
        if offset + size_of::<u64>() < self.data.len() {
            let mut bytes = [0; size_of::<u64>()];
            bytes.copy_from_slice(&self.data[offset..offset + size_of::<u64>()]);
            Some(u64::from_be_bytes(bytes))
        } else {
            None
        }
    }

    pub fn get_with_cells(&self, offset: usize, cells: usize) -> Option<u64> {
        match cells {
            0 => Some(0),
            1 => self.get_u32(offset).map(|v| v as u64),
            2 => self.get_u64(offset),
            _ => None,
        }
    }

    pub fn get_str(&self, index: usize) -> Option<&str> {
        let mut count = 0;
        let mut start = 0;

        for (i, &b) in self.data().iter().enumerate() {
            if b == 0 {
                if count == index {
                    return core::str::from_utf8(&self.data()[start..i]).ok();
                }

                start = i + 1;
                count += 1;
            }
        }

        if count == index {
            core::str::from_utf8(&self.data()[start..]).ok()
        } else {
            None
        }
    }
}

impl Device {
    pub fn new(handle: Handle) -> Self {
        Self { handle }
    }

    pub async fn get_pci_info(&self) -> Result<PciInfo> {
        let head = bragi::head_to_bytes(&bindings::GetPciInfoRequest::new())?;
        let (offer, (_send_head, recv)) = hel::submit_async(
            &self.handle,
            hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
        )
        .await?;

        let recv_data = recv?;
        let conversation_lane = offer?.expect("No lane offered");
        let preamble = bragi::preamble_from_bytes(&recv_data)?;
        let mut tail_buffer = vec![0; preamble.tail_size() as usize];

        hel::submit_async(
            &conversation_lane,
            hel::ReceiveBuffer::new(&mut tail_buffer),
        )
        .await??; // Handle both the submit_async and receive errors

        let response: bindings::SvrResponse =
            bragi::head_tail_from_bytes(&recv_data, &tail_buffer)?;

        if response.error() == bindings::Errors::Success {
            Ok(PciInfo::decode(&response))
        } else {
            Err(Error::from(response.error()))
        }
    }

    pub async fn access_bar(&self, bar: usize) -> Result<Handle> {
        let head = bragi::head_to_bytes(&bindings::AccessBarRequest::new(bar as i32))?;
        let (offer, (_send, recv)) = hel::submit_async(
            &self.handle,
            hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
        )
        .await?;

        let recv_data = recv?;
        let conversation_lane = offer?.expect("No lane offered");
        let preamble = bragi::preamble_from_bytes(&recv_data)?;
        let mut tail_buffer = vec![0; preamble.tail_size() as usize];
        let (_recv, pull) = hel::submit_async(
            &conversation_lane,
            (
                hel::ReceiveBuffer::new(&mut tail_buffer),
                hel::PullDescriptor,
            ),
        )
        .await?;

        let response: bindings::SvrResponse =
            bragi::head_tail_from_bytes(&recv_data, &tail_buffer)?;

        if response.error() == bindings::Errors::Success {
            Ok(pull?.expect("No descriptor pushed"))
        } else {
            Err(Error::from(response.error()))
        }
    }

    pub async fn access_irq(&self, index: usize) -> Result<Handle> {
        let head = bragi::head_to_bytes(&bindings::AccessIrqRequest::new(index as u64))?;
        let (offer, (_send_head, recv)) = hel::submit_async(
            &self.handle,
            hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
        )
        .await?;

        let recv_data = recv?;
        let conversation_lane = offer?.expect("No lane offered");
        let preamble = bragi::preamble_from_bytes(&recv_data)?;
        let mut tail_buffer = vec![0; preamble.tail_size() as usize];
        let (_recv, pull) = hel::submit_async(
            &conversation_lane,
            (
                hel::ReceiveBuffer::new(&mut tail_buffer),
                hel::PullDescriptor,
            ),
        )
        .await?;

        let response: bindings::SvrResponse =
            bragi::head_tail_from_bytes(&recv_data, &tail_buffer)?;

        if response.error() == bindings::Errors::Success {
            Ok(pull?.expect("No descriptor pushed"))
        } else {
            Err(Error::from(response.error()))
        }
    }

    pub async fn get_dt_property(&self, name: &str) -> Result<Option<DtProperty>> {
        let head = bragi::head_to_bytes(&bindings::GetDtPropertyRequest::new(name.to_string()))?;
        let (offer, (_send_head, recv)) = hel::submit_async(
            &self.handle,
            hel::Offer::new((hel::SendBuffer::new(&head), hel::ReceiveInline)),
        )
        .await?;

        let recv_data = recv?;
        let conversation_lane = offer?.expect("No lane offered");
        let preamble = bragi::preamble_from_bytes(&recv_data)?;
        let mut tail_buffer = vec![0; preamble.tail_size() as usize];
        hel::submit_async(
            &conversation_lane,
            hel::ReceiveBuffer::new(&mut tail_buffer),
        )
        .await??; // Handle both the submit_async and receive errors

        let response: bindings::GetDtPropertyResponse =
            bragi::head_tail_from_bytes(&recv_data, &tail_buffer)?;

        if response.error() == bindings::Errors::Success {
            Ok(Some(DtProperty::from_bytes(response.data())))
        } else if response.error() == bindings::Errors::PropertyNotFound {
            Ok(None)
        } else {
            Err(Error::from(response.error()))
        }
    }
}
