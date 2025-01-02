#pragma once

#include <async/mutex.hpp>
#include <netinet/in.h>
#include <protocols/fs/client.hpp>
#include <protocols/mbus/client.hpp>
#include <span>

#include "../controller.hpp"

namespace nvme::fabric {

struct TcpQueue final : public Queue {
	TcpQueue(uint16_t cid, unsigned int index, unsigned int depth, in_addr addr, in_port_t port, helix::BorrowedLane lane, std::span<uint8_t, 16> uuid);

	async::result<void> init() override;
	async::detached run() override;

	async::result<Command::Result> submitCommand(std::unique_ptr<Command> cmd) override;

	uint16_t controllerId() {
		return controllerId_;
	}
private:
	async::result<protocols::fs::Error> connect();
	async::detached keepAlive();
	async::detached submitPendingLoop();
	async::result<void> submitCommandToDevice(std::unique_ptr<Command> cmd);

	in_addr addr_;
	in_port_t port_;
	helix::BorrowedLane lane_;
	uint16_t controllerId_;
	// keepalive timeout value in ms
	size_t keepAliveTimeout_ = 10'000;
	std::span<uint8_t, 16> uuid_;

	std::vector<std::byte> buf_{8256};

	async::oneshot_event connectedEvent_;

	std::unique_ptr<protocols::fs::File> file_;

	async::mutex sendMutex;
};

struct Tcp final : public Controller {
	Tcp(mbus_ng::EntityId entity, in_addr addr, in_port_t port, helix::UniqueLane netserver);

	async::detached run() override;
	async::result<Command::Result> submitIoCommand(std::unique_ptr<Command> cmd) override;

private:
	async::result<frg::expected<spec::CompletionStatus, uint64_t>> fabricGetProperty(uint32_t propertyOffset, size_t size);
	async::result<frg::expected<spec::CompletionStatus, uint64_t>> fabricSetProperty(uint32_t propertyOffset, uint64_t value, size_t size);

	in_addr serverAddr_;
	in_port_t serverPort_;
	helix::UniqueLane netserverLane_;
};

} // namespace nvme::fabric
