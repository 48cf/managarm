#pragma once

#include <cstdint>

#include <arch/mem_space.hpp>
#include <async/result.hpp>
#include <helix/memory.hpp>
#include <protocols/hw/client.hpp>

#include <mailboxd/mailbox.hpp>

namespace mailboxd {

struct [[gnu::packed]] AppleMailboxMessage {
	uint64_t msg0;
	uint32_t msg1;
};

struct AppleAscMailboxV4 final : Mailbox {
	AppleAscMailboxV4(std::string location, helix::Mapping mapping, helix::UniqueIrq irq);

	static async::result<std::shared_ptr<AppleAscMailboxV4>> create(protocols::hw::Device device);

	std::optional<uint32_t> translateChannel(std::span<const uint32_t> specifier) override;
	std::optional<Channel> requestChannel(uint32_t channel_id) override;

	async::result<void> send(uint32_t channel, const void *data, size_t size) override;
	async::result<void> receive(uint32_t channel, void *data, size_t size) override;

private:
	async::detached _handleIrqs();

private:
	std::string _location;

	helix::Mapping _mapping;
	helix::UniqueIrq _recvNotEmptyIrq;

	arch::mem_space _mmio;
};

} // namespace mailboxd
