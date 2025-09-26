#include <print>

#include <bragi/helpers-all.hpp>
#include <bragi/helpers-std.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>

#include "hw.bragi.hpp"

#include <mailboxd/apple.hpp>

namespace {

async::detached handleChannelRequests(helix::UniqueLane lane, mailboxd::Channel channel) {
	while (true) {
		auto [accept, recvHead] =
		    co_await helix_ng::exchangeMsgs(lane, helix_ng::accept(helix_ng::recvInline()));
		if (accept.error() == kHelErrLaneShutdown || accept.error() == kHelErrEndOfLane) {
			co_return;
		}
		HEL_CHECK(accept.error());
		HEL_CHECK(recvHead.error());

		auto conversation = accept.descriptor();
		auto preamble = bragi::read_preamble(recvHead);
		recvHead.reset();
		assert(!preamble.error());

		if (preamble.id() == bragi::message_id<managarm::hw::MailboxMessage>) {
			std::vector<std::byte> tailBuffer(preamble.tail_size());
			auto [recvTail] = co_await helix_ng::exchangeMsgs(
			    conversation, helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
			);
			HEL_CHECK(recvTail.error());

			auto req = bragi::parse_head_tail<managarm::hw::MailboxMessage>(recvHead, tailBuffer);
			recvHead.reset();
			if (!req) {
				std::println("mailboxd: Ignoring request due to decoding failure.\n");
				continue;
			}

			co_await channel.mbox->send(channel.id, req->data().data(), req->data().size());
		} else if (preamble.id() == bragi::message_id<managarm::hw::MailboxReceive>) {
			auto req = bragi::parse_head_only<managarm::hw::MailboxReceive>(recvHead);
			recvHead.reset();
			if (!req) {
				std::println("mailboxd: Ignoring request due to decoding failure.\n");
				continue;
			}

			std::vector<uint8_t> buffer(req->msg_size());
			co_await channel.mbox->receive(channel.id, buffer.data(), buffer.size());

			auto [sendBuffer] = co_await helix_ng::exchangeMsgs(
			    conversation, helix_ng::sendBuffer(buffer.data(), buffer.size())
			);
			HEL_CHECK(sendBuffer.error());
		}
	}
}

async::detached
handleMailboxRequests(helix::UniqueLane lane, std::shared_ptr<mailboxd::Mailbox> device) {
	while (true) {
		auto [accept, recvHead] =
		    co_await helix_ng::exchangeMsgs(lane, helix_ng::accept(helix_ng::recvInline()));
		if (accept.error() == kHelErrLaneShutdown || accept.error() == kHelErrEndOfLane) {
			co_return;
		}
		HEL_CHECK(accept.error());
		HEL_CHECK(recvHead.error());

		auto conversation = accept.descriptor();
		auto preamble = bragi::read_preamble(recvHead);
		assert(!preamble.error());

		if (preamble.id() == bragi::message_id<managarm::hw::AccessMailboxRequest>) {
			std::vector<uint8_t> tailBuffer(preamble.tail_size());
			auto [recvTail] = co_await helix_ng::exchangeMsgs(
			    conversation, helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
			);
			HEL_CHECK(recvTail.error());

			auto req =
			    bragi::parse_head_tail<managarm::hw::AccessMailboxRequest>(recvHead, tailBuffer);
			recvHead.reset();
			if (!req) {
				std::println("mailboxd: Ignoring request due to decoding failure.\n");
				continue;
			}

			auto channelId = device->translateChannel(req->specifier());
			assert(channelId && "Channel translation failed");

			auto channel = device->requestChannel(*channelId);
			assert(channel && "Channel request failed");

			managarm::hw::AccessMailboxResponse resp;
			resp.set_channel_id(channel->id);

			auto [localLane, remoteLane] = helix::createStream();
			auto [send, push] = co_await helix_ng::exchangeMsgs(
			    conversation,
			    helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			    helix_ng::pushDescriptor(std::move(remoteLane))
			);
			HEL_CHECK(send.error());
			HEL_CHECK(push.error());

			handleChannelRequests(std::move(localLane), std::move(*channel));
		} else {
			std::println("mailboxd: Ignoring unknown request type {}\n", preamble.id());
		}
	}
}

async::detached
handleMbusRequests(mbus_ng::EntityManager entity, std::shared_ptr<mailboxd::Mailbox> device) {
	while (true) {
		auto [localLane, remoteLane] = helix::createStream();

		// If this fails, too bad!
		(co_await entity.serveRemoteLane(std::move(remoteLane))).unwrap();

		handleMailboxRequests(std::move(localLane), device);
	}
}

async::detached asyncMain() {
	auto filter = mbus_ng::EqualsFilter{"unix.subsystem", "dt"};
	auto enumerator = mbus_ng::Instance::global().enumerate(filter);

	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created) {
				continue;
			}

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);

			uint32_t phandle;
			{
				auto phandleStrProp = event.properties.find("dt.phandle");
				if (phandleStrProp == event.properties.end()) {
					std::println("mailboxd: Device has no phandle, ignoring");
					continue;
				}

				auto phandleString = std::get_if<mbus_ng::StringItem>(&phandleStrProp->second);
				if (!phandleString) {
					std::println("mailboxd: Device phandle is not a string, ignoring");
					continue;
				}

				auto phandleStr = phandleString->value;
				auto res = std::from_chars(
				    phandleStr.data(), phandleStr.data() + phandleStr.size(), phandle, 16
				);
				if (res.ec == std::errc::invalid_argument) {
					continue;
				}
			}

			std::shared_ptr<mailboxd::Mailbox> device;
			if (event.properties.contains("dt.compatible=apple,asc-mailbox-v4")) {
				std::println("mailboxd: Found Apple ASC Mailbox V4 device");

				auto lane = (co_await entity.getRemoteLane()).unwrap();
				auto hwDevice = protocols::hw::Device(std::move(lane));

				device = co_await mailboxd::AppleAscMailboxV4::create(std::move(hwDevice));
			}

			if (!device) {
				continue;
			}

			mbus_ng::Properties properties{
			    {"class", mbus_ng::StringItem{"mailbox"}},
			    {"mbox.phandle", mbus_ng::StringItem{std::format("{:x}", phandle)}}
			};
			auto entityManager =
			    (co_await mbus_ng::Instance::global().createEntity("mailbox", properties)).unwrap();

			handleMbusRequests(std::move(entityManager), std::move(device));
		}
	}
}

} // namespace

int main() {
	std::println("mailboxd: Starting up");

	asyncMain();

	async::run_forever(helix::currentDispatcher);
}
