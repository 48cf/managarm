use super::{Error, Item, Properties, Result, bindings};

pub struct EntityManager {
    entity: Entity,
    management_lane: hel::Handle,
}

impl EntityManager {
    pub async fn serve_remote_lane(&self, lane: hel::Handle) -> Result<()> {
        let head = bragi::head_to_bytes(&bindings::ServeRemoteLaneRequest::new())?;

        let (offer, (_send,)) = hel::submit_async(
            &self.management_lane,
            hel::Offer::new_with_lane((hel::SendBuffer::new(&head),)),
        )
        .await?;
        println!("have lane");
        let offer_lane = offer?.expect("No lane offered");
        let push = hel::submit_async(&offer_lane, hel::PushDescriptor::new(lane)).await??;
        println!("pushed descriptor");
        let recv = hel::submit_async(&offer_lane, hel::ReceiveInline).await?;
        println!("received inline response");

        // let (_offer, (_send_head, _push, recv)) = hel::submit_async(
        //     &self.management_lane,
        //     hel::Offer::new((
        //         hel::SendBuffer::new(&head),
        //         hel::PushDescriptor::new(lane),
        //         hel::ReceiveInline,
        //     )),
        // )
        // .await?;

        let recv_data = recv?;
        let response: bindings::ServeRemoteLaneResponse = bragi::head_from_bytes(&recv_data)?;

        if response.error() == bindings::Error::Success {
            Ok(())
        } else {
            Err(Error::from(response.error()))
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct Entity {
    id: i64,
}

impl Entity {
    pub fn from_id(id: i64) -> Self {
        Self { id }
    }

    pub async fn create(name: &str, properties: &Properties) -> Result<EntityManager> {
        let (head, tail) = bragi::head_tail_to_bytes(&bindings::CreateObjectRequest::new(
            name.to_string(),
            properties
                .iter()
                .map(|(key, value)| {
                    let name = key.clone();
                    let item = value.encode_item();

                    bindings::Property::new(name, item)
                })
                .collect(),
        ))?;
        let (_offer, (_send_head, _send_tail, recv, pull)) = hel::submit_async(
            crate::posix::mbus_lane_handle(),
            hel::Offer::new((
                hel::SendBuffer::new(&head),
                hel::SendBuffer::new(&tail),
                hel::ReceiveInline,
                hel::PullDescriptor,
            )),
        )
        .await?;

        let recv_data = recv?;
        let descriptor = pull?.expect("No descriptor pushed");
        let response: bindings::CreateObjectResponse = bragi::head_from_bytes(&recv_data)?;

        if response.error() == bindings::Error::Success {
            Ok(EntityManager {
                entity: Self::from_id(response.id()),
                management_lane: descriptor,
            })
        } else {
            Err(Error::from(response.error()))
        }
    }

    pub async fn get_remote_lane(&self) -> Result<hel::Handle> {
        let head = bragi::head_to_bytes(&bindings::GetRemoteLaneRequest::new(self.id))?;
        let (_offer, (_send_head, recv, pull)) = hel::submit_async(
            crate::posix::mbus_lane_handle(),
            hel::Offer::new((
                hel::SendBuffer::new(&head),
                hel::ReceiveInline,
                hel::PullDescriptor,
            )),
        )
        .await?;

        let recv_data = recv?;
        let response: bindings::GetRemoteLaneResponse = bragi::head_from_bytes(&recv_data)?;

        if response.error() == bindings::Error::Success {
            Ok(pull?.expect("No descriptor pushed"))
        } else {
            Err(Error::from(response.error()))
        }
    }

    pub async fn get_properties(&self) -> Result<Properties> {
        let head = bragi::head_to_bytes(&bindings::GetPropertiesRequest::new(self.id))?;
        let (offer, (_send_head, recv)) = hel::submit_async(
            crate::posix::mbus_lane_handle(),
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

        let response: bindings::GetPropertiesResponse =
            bragi::head_tail_from_bytes(&recv_data, &tail_buffer)?;

        if response.error() == bindings::Error::Success {
            Ok(response
                .properties()
                .iter()
                .map(|property| {
                    let item = Item::decode_item(property.item());
                    let name = property.name().to_string();

                    (name, item)
                })
                .collect())
        } else {
            Err(Error::from(response.error()))
        }
    }

    pub async fn update_properties(&self, properties: Properties) -> Result<()> {
        let request = bindings::UpdatePropertiesRequest::new(
            self.id,
            properties
                .iter()
                .map(|(key, value)| bindings::Property::new(key.clone(), value.encode_item()))
                .collect(),
        );

        let (head, tail) = bragi::head_tail_to_bytes(&request)?;
        let (_offer, (_send_head, _send_tail, recv)) = hel::submit_async(
            crate::posix::mbus_lane_handle(),
            hel::Offer::new_with_lane((
                hel::SendBuffer::new(&head),
                hel::SendBuffer::new(&tail),
                hel::ReceiveInline,
            )),
        )
        .await?;

        let recv_data = recv?;
        let response: bindings::GetRemoteLaneResponse = bragi::head_from_bytes(&recv_data)?;

        if response.error() == bindings::Error::Success {
            Ok(())
        } else {
            Err(Error::from(response.error()))
        }
    }
}
