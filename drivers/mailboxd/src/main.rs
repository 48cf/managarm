// use std::time::Duration;

mod apple;

use std::{
    collections::HashMap,
    sync::{Arc, LazyLock},
};

use async_lock::RwLock;
use bragi::Message;
use managarm::{
    hw::{self, Device},
    mbus::{self, Entity, Enumerator, EventType, Filter, Item},
};

pub trait MailboxDevice {
    fn translate_channel(&self, specifier: &[u32]) -> Option<u32>;
    fn request_channel(&self, channel: u32) -> Option<MailboxChannel>;

    fn send(&self, channel: &MailboxChannel, buffer: &[u8]);
    fn receive(&self, channel: &MailboxChannel, buffer: &mut [u8]);
}

pub struct MailboxChannel {
    device: Arc<dyn MailboxDevice + Send + Sync>,
    channel: u32,
}

async fn handle_channel_requests(lane: hel::Handle, channel: MailboxChannel) {
    println!(
        "mailboxd: Handling requests for channel {}",
        channel.channel
    );
    todo!()
}

async fn handle_device_requests(lane: hel::Handle, device: Arc<dyn MailboxDevice + Send + Sync>) {
    loop {
        println!("mailboxd: Waiting for device request");

        let (accept, recv) = hel::submit_async(&lane, hel::Accept::new((hel::ReceiveInline)))
            .await
            .expect("Failed to submit receive action");

        println!("mailboxd: Got device request");

        let conversation_lane = accept.expect("Accept failed").expect("No lane accepted");
        let recv_data = recv.expect("Failed to receive data");
        let preamble = bragi::preamble_from_bytes(&recv_data).expect("Failed to parse preamble");

        if preamble.id() == managarm::hw::bindings::AccessMailboxRequest::MESSAGE_ID {
            let mut tail_buffer = vec![0; preamble.tail_size() as usize];
            hel::submit_async(
                &conversation_lane,
                hel::ReceiveBuffer::new(&mut tail_buffer),
            )
            .await
            .expect("Failed to submit receive action")
            .expect("Failed to receive tail data");

            let request: hw::bindings::AccessMailboxRequest =
                bragi::head_tail_from_bytes(&recv_data, &tail_buffer)
                    .expect("Failed to parse request");

            let channel = device
                .translate_channel(request.specifier())
                .expect("Invalid channel specifier");

            let mbox_channel = device
                .request_channel(channel)
                .expect("Failed to request channel");

            let response_bytes =
                bragi::head_to_bytes(&managarm::hw::bindings::AccessMailboxResponse::new(channel))
                    .expect("Failed to serialize response");

            let (local, remote) = hel::create_stream(false).expect("Failed to create stream");
            let (_send, _push) = hel::submit_async(
                &conversation_lane,
                (
                    hel::SendBuffer::new(&response_bytes),
                    hel::PushDescriptor::new(remote),
                ),
            )
            .await
            .expect("Failed to submit send action");

            hel::spawn(handle_channel_requests(local, mbox_channel));
        } else {
            panic!("mailboxd: Unknown request ID {}", preamble.id());
        }
    }
}

async fn async_main() -> anyhow::Result<()> {
    let mut enumerator = Enumerator::new(Filter::Equals("unix.subsystem", "dt"));

    loop {
        let (_, events) = enumerator.next_events().await?;

        for event in events
            .iter()
            .filter(|e| e.event_type() == EventType::Created)
        {
            let properties = event.properties();
            let device = if properties.contains_key("dt.compatible=apple,asc-mailbox-v4") {
                println!("mailboxd: Found Apple ASC Mailbox V4 device");

                let lane = event.entity().get_remote_lane().await?;
                let hw_device = Device::new(lane);

                Some(apple::AppleAscMailboxV4::new(hw_device).await?)
            } else {
                None
            };

            if let Some(device) = device {
                let phandle = properties
                    .get("dt.phandle")
                    .and_then(|p| {
                        if let Item::String(s) = p {
                            u32::from_str_radix(s, 16).ok()
                        } else {
                            None
                        }
                    })
                    .ok_or(anyhow::anyhow!(
                        "mailboxd: Device is missing phandle property"
                    ))?;

                let mut new_props = HashMap::new();

                new_props.insert("class".into(), Item::String("mailbox".into()));
                new_props.insert("mbox.phandle".into(), Item::String(format!("{phandle:x}")));

                let manager = Entity::create("mailbox", &new_props).await?;

                hel::spawn(async move {
                    loop {
                        println!("mailboxd: Waiting for mailbox entity request");

                        let (local, remote) =
                            hel::create_stream(false).expect("Failed to create stream");

                        println!("mailboxd: Created stream for mailbox entity request");

                        manager
                            .serve_remote_lane(remote)
                            .await
                            .expect("Failed to serve lane");

                        println!("mailboxd: Serving mailbox entity request");

                        hel::spawn(handle_device_requests(local, device.clone()));
                    }
                });
            }
        }
    }

    Ok(())
}

fn main() -> anyhow::Result<()> {
    println!("mailboxd: Hello world!");

    hel::block_on(async_main())??;

    Ok(())
}
