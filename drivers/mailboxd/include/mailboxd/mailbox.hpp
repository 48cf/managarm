#pragma once

#include <cstdint>
#include <memory>

#include <async/result.hpp>

namespace mailboxd {

struct Channel;

struct Mailbox : std::enable_shared_from_this<Mailbox> {
	virtual ~Mailbox() = default;

	virtual std::optional<uint32_t> translateChannel(std::span<const uint32_t> specifier) = 0;
	virtual std::optional<Channel> requestChannel(uint32_t channel_id) = 0;

	virtual async::result<void> send(uint32_t channel, const void *data, size_t size) = 0;
	virtual async::result<void> receive(uint32_t channel, void *data, size_t size) = 0;
};

struct Channel {
	Channel(std::shared_ptr<Mailbox> mailbox, uint32_t id) : mbox{std::move(mailbox)}, id{id} {}

	std::shared_ptr<Mailbox> mbox;
	uint32_t id;
};

} // namespace mailboxd
