#include <print>

#include <apple/rtkit.hpp>
#include <mailboxd/apple.hpp>

namespace {

constexpr uint8_t kRtkEpMgmt = 0x0;
constexpr uint8_t kRtkEpCrashlog = 0x1;
constexpr uint8_t kRtkEpSyslog = 0x2;
constexpr uint8_t kRtkEpDebug = 0x3;
constexpr uint8_t kRtkEpIoReport = 0x4;
constexpr uint8_t kRtkEpOslog = 0x8;
constexpr uint8_t kRtkEpTracekit = 0xa;
constexpr uint8_t kRtkEpApp = 0x20;

constexpr uint8_t kRtkPowerStateOff = 0x0;
constexpr uint8_t kRtkPowerStateSleep = 0x1;
constexpr uint8_t kRtkPowerStateIdle = 0x201;
constexpr uint8_t kRtkPowerStateQuiesced = 0x10;
constexpr uint8_t kRtkPowerStateOn = 0x20;
constexpr uint8_t kRtkPowerStateInit = 0x220;

constexpr uint8_t kMgmtHello = 0x1;
constexpr uint8_t kMgmtHelloReply = 0x2;
constexpr uint8_t kMgmtStartEndpoint = 0x5;
constexpr uint8_t kMgmtSetIopPowerState = 0x6;
constexpr uint8_t kMgmtSetIopPowerStateReply = 0x7;
constexpr uint8_t kMgmtMapEndpoint = 0x8;
constexpr uint8_t kMgmtMapEndpointReply = 0x8;
constexpr uint8_t kMgmtSetApPowerState = 0xb;
constexpr uint8_t kMgmtSetApPowerStateReply = 0xb;

constexpr uint8_t kRtkBufferRequest = 0x1;
constexpr uint8_t kRtkBufferRequestReply = 0x1;

struct RtKitSyslogLog {
	uint32_t hdr;
	uint32_t unk;
	char context[24];
	char msg[];
};

struct RtKitCrashlogHeader {
	uint32_t type;
	uint32_t ver;
	uint32_t total_size;
	uint32_t flags;
	uint8_t _padding[16];
};

struct RtKitCrashlogEntry {
	uint32_t type;
	uint32_t _padding;
	uint32_t flags;
	uint32_t len;
	char payload[];
};

} // namespace

namespace apple {

RtKit::RtKit(protocols::hw::MailboxChannel channel, RtKitOperations *ops)
: _channel{std::move(channel)},
  _ops{ops} {
	_messageRxLoop();
}

async::result<void> RtKit::boot() {
	co_await _sendManagementMessage(kMgmtSetApPowerState, kRtkPowerStateOn);
	co_await _apPowerStateReplyEvent.wait();
}

async::result<void> RtKit::_sendMessage(uint8_t endpoint, uint64_t message) {
	mailboxd::AppleMailboxMessage msg;
	msg.msg0 = message;
	msg.msg1 = endpoint;
	co_await _channel.sendMessage(&msg, sizeof(msg));
}

async::result<void> RtKit::_sendManagementMessage(uint8_t type, uint64_t message) {
	co_await _sendMessage(kRtkEpMgmt, ((uint64_t)type << 52) | message);
}

async::result<void> RtKit::_startEndpoint(uint8_t endpoint) {
	co_await _sendManagementMessage(kMgmtStartEndpoint, ((uint64_t)endpoint << 32) | (1 << 1));
}

async::detached RtKit::_messageRxLoop() {
	while (true) {
		mailboxd::AppleMailboxMessage msg;
		co_await _channel.receiveMessage(&msg, sizeof(msg));

		auto message = msg.msg0;
		auto endpoint = msg.msg1 & 0xff;

		// std::println("apple-rtkit: Received message on endpoint {:#x}: {:#x}", endpoint,
		// message);

		if (endpoint == kRtkEpMgmt) {
			co_await _handleManagementMessage(message);
		} else if (endpoint == kRtkEpCrashlog) {
			co_await _handleCrashlogMessage(message);
		} else if (endpoint == kRtkEpSyslog) {
			co_await _handleSyslogMessage(message);
		} else if (endpoint == kRtkEpIoReport) {
			auto type = (message >> 52) & 0xff;

			if (type == kRtkBufferRequest) {
				co_await _handleBufferRequest(kRtkEpIoReport, message, _rtkIoReportBuffer);
			} else if (type == 8 || type == 12) {
				// Unknown, must be ACKed or the co-processor will hang.
				co_await _sendMessage(kRtkEpIoReport, message);
			} else {
				std::println("apple-rtkit: Received ioreport message of unknown type {}", type);
				assert(false);
			}
		} else if (endpoint == kRtkEpOslog) {
			auto type = (message >> 56) & 0xff;

			if (type == kRtkBufferRequest) {
				co_await _handleBufferRequest(kRtkEpOslog, message, _rtkOslogBuffer);
			} else {
				std::println("apple-rtkit: Received oslog message of unknown type {}", type);
				assert(false);
			}
		} else if (endpoint >= kRtkEpApp) {
			std::println("apple-rtkit: Received message for application endpoint {}", endpoint);
			assert(false);
		} else {
			std::println("apple-rtkit: Received message for unknown endpoint {}", endpoint);
			assert(false);
		}
	}
}

async::result<void> RtKit::_handleManagementMessage(uint64_t message) {
	auto type = (message >> 52) & 0xff;

	if (type == kMgmtHello) {
		constexpr uint64_t minSupportedVersion = 11;
		constexpr uint64_t maxSupportedVersion = 12;

		auto minVer = message & 0xffff;
		auto maxVer = (message >> 16) & 0xffff;

		std::println("apple-rtkit: Received hello message, minVer={}, maxVer={}", minVer, maxVer);

		if (minVer > maxSupportedVersion) {
			std::println("apple-rtkit: Firmware minimum version {} is too new", minVer);
			assert(false);
		}

		if (maxVer < minSupportedVersion) {
			std::println("apple-rtkit: Firmware maximum version {} is too old", maxVer);
			assert(false);
		}

		auto agreedVersion = frg::min(maxVer, maxSupportedVersion) & 0xffff;
		auto reply = agreedVersion | (agreedVersion << 16);

		std::println("apple-rtkit: Agreed on protocol version {}", agreedVersion);

		co_await _sendManagementMessage(kMgmtHelloReply, reply);
	} else if (type == kMgmtMapEndpoint) {
		auto bitmap = message & 0xffffffff;
		auto base = (message >> 32) & 0x7;
		auto isLast = (message >> 51) & 0x1;

		for (size_t i = 0; i < sizeof(uint32_t) * CHAR_BIT; i++) {
			if (!(bitmap & (1 << i))) {
				continue;
			}

			std::println("apple-rtkit: Discovered endpoint {:#x}", base * 0x20 + i);
			_endpointsActive.set(base * 0x20 + i);
		}

		uint64_t reply = base << 32;

		if (isLast) {
			reply |= UINT64_C(1) << 51;
		} else {
			reply |= 1 << 0;
		}

		co_await _sendManagementMessage(kMgmtMapEndpointReply, reply);

		if (!isLast) {
			co_return;
		}

		for (size_t i = 0; i < kRtkEpApp; i++) {
			if (i == kRtkEpMgmt || _endpointsActive.test(i)) {
				co_await _startEndpoint(i);
			}
		}

		_mapEndpointsEvent.raise();
	} else if (type == kMgmtSetIopPowerStateReply) {
		auto newState = message & 0xffff;
		std::println(
		    "apple-rtkit: IOP power state changed from {:#x} to {:#x}", _iopPowerState, newState
		);
		_iopPowerState = newState;
		_iopPowerStateReplyEvent.raise();
	} else if (type == kMgmtSetApPowerStateReply) {
		auto newState = message & 0xffff;
		std::println(
		    "apple-rtkit: AP power state changed from {:#x} to {:#x}", _apPowerState, newState
		);
		_apPowerState = newState;
		_apPowerStateReplyEvent.raise();
	} else {
		std::println(
		    "apple-rtkit: Received unknown management message of type {}: {:#x}", type, message
		);
	}
}

async::result<void> RtKit::_handleCrashlogMessage(uint64_t message) {
	auto type = (message >> 52) & 0xff;

	if (type != 1) {
		std::println("apple-rtkit: Received crashlog message of unknown type {}", type);
		co_return;
	}

	if (_rtkCrashlogBuffer.size == 0) {
		co_await _handleBufferRequest(kRtkEpCrashlog, message, _rtkCrashlogBuffer);
		co_return;
	}

	std::println("apple-rtkit: Co-processor has crashed :(");

	auto header = (RtKitCrashlogHeader *)_rtkCrashlogBuffer.buffer;

	if (header->type != 'CLHE') {
		std::println("apple-rtkit: Bad crashlog header {:x}", header->type);
		assert(false);
	}

	auto entry = (RtKitCrashlogEntry *)(_rtkCrashlogBuffer.buffer + sizeof(RtKitCrashlogHeader));

	std::println("apple-rtkit: Crash info:");

	while (entry->type != 'CLHE') {
		if (entry->type == 'Cstr') {
			std::println("- {}", (const char *)&entry->payload[4]);
		} else {
			std::println("- {:#x}", entry->type);
		}

		entry = (RtKitCrashlogEntry *)((uintptr_t)entry + entry->len);
	}
}

async::result<void> RtKit::_handleSyslogMessage(uint64_t message) {
	auto type = (message >> 52) & 0xff;

	if (type == kRtkBufferRequest) {
		co_await _handleBufferRequest(kRtkEpSyslog, message, _rtkSyslogBuffer);
	} else if (type == 5) {
		auto index = message & 0xff;
		auto stride = _syslogEntrySize + sizeof(RtKitSyslogLog);
		auto log = (RtKitSyslogLog *)(_rtkSyslogBuffer.buffer + index * stride);
		std::println("apple-rtkit: Syslog entry [{}] {}", &log->context[0], &log->msg[0]);
		co_await _sendMessage(kRtkEpSyslog, message);
	} else if (type == 8) {
		_syslogCount = message & 0xffff;
		_syslogEntrySize = (message >> 24) & 0xffff;
		std::println(
		    "apple-rtkit: Syslog configured, count={}, entrySize={}", _syslogCount, _syslogEntrySize
		);
	} else {
		std::println(
		    "apple-rtkit: Received unknown syslog message of type {}: {:#x}", type, message
		);
	}
}

async::result<void>
RtKit::_handleBufferRequest(uint8_t endpoint, uint64_t message, RtKitBuffer &buffer) {
	uint64_t iova;
	uint64_t size;

	if (endpoint == kRtkEpOslog) {
		iova = (message & 0xfffffffff) << 12;
		size = (message >> 36) & 0xfffff;
	} else {
		iova = message & 0x3ffffffffff;
		size = ((message >> 44) & 0xff) << 12;
	}

	std::println("apple-rtkit: Buffer request, iova={:#x}, size={}", iova, size);

	buffer.deviceAddress = iova;
	buffer.size = size;
	buffer.endpoint = endpoint;
	buffer.isMapped = iova != 0;

	if (_ops->shmemSetup) {
		if (!_ops->shmemSetup(_ops->arg, buffer)) {
			std::println("apple-rtkit: Failed to set up crashlog shared memory");
			assert(false);
		}
	} else if (!buffer.isMapped) {
		auto allocateWithAlignment = [](size_t size, size_t alignment) {
			size_t alignedSize = (size + alignment - 1) & ~(alignment - 1);

			HelHandle memory;
			HEL_CHECK(helAllocateMemory(alignedSize, kHelAllocContinuous, nullptr, &memory));

			void *address;
			HEL_CHECK(helMapMemory(
			    memory,
			    kHelNullHandle,
			    nullptr,
			    0,
			    alignedSize,
			    kHelMapProtRead | kHelMapProtWrite,
			    &address
			));
			HEL_CHECK(helCloseDescriptor(kHelThisUniverse, memory));

			auto physical = helix::ptrToPhysical(address);
			assert(!(physical & (alignment - 1)));

			return (uintptr_t)address;
		};

		auto allocation = allocateWithAlignment(buffer.size, 0x4000);
		auto physical = helix::addressToPhysical(allocation);

		buffer.buffer = allocation;
		buffer.deviceAddress = physical;
	}

	assert(!(buffer.deviceAddress & UINT64_C(0x3fff)));
	if (buffer.isMapped) {
		// TODO
		std::println(
		    "apple-rtkit: Using pre-mapped shared memory at {:#x}, size={}",
		    buffer.deviceAddress,
		    buffer.size
		);
		assert(false);
	}

	if (endpoint == kRtkEpOslog) {
		uint64_t reply = (uint64_t)kRtkBufferRequestReply << 56;
		reply |= (buffer.deviceAddress >> 12) & 0xfffffffff;
		reply |= (size & 0xfffff) << 36;
		co_await _sendMessage(endpoint, reply);
	} else {
		uint64_t reply = (uint64_t)kRtkBufferRequestReply << 52;
		reply |= (buffer.deviceAddress & 0x3ffffffffff);
		reply |= (uint64_t)((buffer.size >> 12) & 0xff) << 44;
		co_await _sendMessage(endpoint, reply);
	}
}

} // namespace apple
