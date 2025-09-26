#pragma once

#include <bitset>

#include <arch/dma_pool.hpp>
#include <async/oneshot-event.hpp>
#include <protocols/hw/client.hpp>

namespace apple {

struct RtKitBuffer {
	bool isMapped{false};
	uint8_t endpoint{0};
	uint64_t buffer{0};
	uint64_t deviceAddress{0};
	size_t size{0};
};

struct RtKitOperations {
	void *arg;

	bool (*shmemSetup)(void *arg, RtKitBuffer &buffer) = nullptr;
	void (*shmemDestroy)(void *arg, RtKitBuffer &buffer) = nullptr;
};

struct RtKit {
	RtKit(protocols::hw::MailboxChannel channel, RtKitOperations *ops);

	async::result<void> boot();

private:
	async::result<void> _sendMessage(uint8_t endpoint, uint64_t message);

	async::result<void> _sendManagementMessage(uint8_t type, uint64_t message);
	async::result<void> _startEndpoint(uint8_t endpoint);

	async::detached _messageRxLoop();

	async::result<void> _handleManagementMessage(uint64_t message);
	async::result<void> _handleCrashlogMessage(uint64_t message);
	async::result<void> _handleSyslogMessage(uint64_t message);

	async::result<void>
	_handleBufferRequest(uint8_t endpoint, uint64_t message, RtKitBuffer &buffer);

private:
	protocols::hw::MailboxChannel _channel;
	RtKitOperations *_ops;

	RtKitBuffer _rtkCrashlogBuffer;
	RtKitBuffer _rtkSyslogBuffer;
	RtKitBuffer _rtkIoReportBuffer;
	RtKitBuffer _rtkOslogBuffer;

	async::oneshot_event _mapEndpointsEvent;
	async::oneshot_event _iopPowerStateReplyEvent;
	async::oneshot_event _apPowerStateReplyEvent;

	size_t _syslogCount{0};
	size_t _syslogEntrySize{0};

	uint16_t _iopPowerState{0};
	uint16_t _apPowerState{0};

	arch::contiguous_pool _dmaPool{arch::contiguous_pool_options{.addressBits = 64}};

	std::bitset<0x100> _endpointsActive;
};

} // namespace apple
