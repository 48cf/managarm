#include <print>

#include <helix/timer.hpp>

#include <mailboxd/apple.hpp>

namespace {

namespace apple_asc_v4 {

constexpr arch::bit_register<uint32_t> cpuControl{0x44};

constexpr arch::bit_register<uint32_t> a2iStatus{0x110};
constexpr arch::bit_register<uint32_t> i2aStatus{0x114};

constexpr arch::scalar_register<uint64_t> a2iMsg0{0x800};
constexpr arch::bit_register<uint64_t> a2iMsg1{0x808};
constexpr arch::scalar_register<uint64_t> i2aMsg0{0x830};
constexpr arch::bit_register<uint64_t> i2aMsg1{0x838};

namespace cpu_control {

constexpr arch::field<uint32_t, bool> start{4, 1};

} // namespace cpu_control

namespace status {

constexpr arch::field<uint32_t, bool> empty{17, 1};
constexpr arch::field<uint32_t, bool> full{16, 1};

} // namespace status

namespace msg1 {

constexpr arch::field<uint64_t, uint32_t> msg{0, 32};
constexpr arch::field<uint64_t, uint8_t> inPtr{40, 4};
constexpr arch::field<uint64_t, uint8_t> outPtr{44, 4};
constexpr arch::field<uint64_t, uint8_t> inCnt{48, 4};
constexpr arch::field<uint64_t, uint8_t> outCnt{52, 5};

} // namespace msg1

} // namespace apple_asc_v4

} // namespace

namespace mailboxd {

AppleAscMailboxV4::AppleAscMailboxV4(
    std::string location, helix::Mapping mapping, helix::UniqueIrq irq
)
: _location{std::move(location)},
  _mapping{std::move(mapping)},
  _recvNotEmptyIrq{std::move(irq)},
  _mmio{_mapping.get()} {
	// Boot the IOP if it isn't already running.
	_mmio.store(apple_asc_v4::cpuControl, apple_asc_v4::cpu_control::start(true));

	// Kick off the IRQ handler.
	_handleIrqs();
}

async::result<std::shared_ptr<AppleAscMailboxV4>>
AppleAscMailboxV4::create(protocols::hw::Device device) {
	auto dtInfo = co_await device.getDtInfo();
	auto reg = co_await device.accessDtRegister(0);
	auto mapping = helix::Mapping{std::move(reg), dtInfo.regs[0].offset, dtInfo.regs[0].length};
	auto location = std::format("dt.{:x}", dtInfo.regs[0].address);

	// Enable the recv-not-empty interrupt
	size_t recvNotEmptyIrq = 3; // Assume it's the 4th interrupt

	auto intNamesProperty = co_await device.getDtProperty("interrupt-names");
	if (intNamesProperty) {
		recvNotEmptyIrq = (size_t)-1;

		for (size_t i = 0; i < dtInfo.numIrqs; i++) {
			auto name = intNamesProperty->asString(i);
			if (name == "recv-not-empty") {
				recvNotEmptyIrq = i;
				break;
			}
		}

		if (recvNotEmptyIrq == (size_t)-1) {
			std::println("apple-asc {}: Failed to find recv-not-empty interrupt", location);
			co_return nullptr;
		}

		std::println(
		    "apple-asc {}: Found recv-not-empty interrupt at index {}", location, recvNotEmptyIrq
		);
	} else {
		std::println(
		    "apple-asc {}: Device has no interrupt-names property, assuming recv-not-empty is "
		    "interrupt #3",
		    location
		);
	}

	co_await device.enableBusIrq();
	auto irq = co_await device.installDtIrq(recvNotEmptyIrq);

	co_return std::make_shared<AppleAscMailboxV4>(
	    std::move(location), std::move(mapping), std::move(irq)
	);
}

std::optional<uint32_t> AppleAscMailboxV4::translateChannel(std::span<const uint32_t> specifier) {
	if (specifier.size() != 0) {
		return std::nullopt;
	}

	// Apple mailbox only has a single channel.
	return {0};
}

std::optional<Channel> AppleAscMailboxV4::requestChannel(uint32_t channel_id) {
	if (channel_id != 0) {
		return std::nullopt;
	}

	return Channel{shared_from_this(), channel_id};
}

async::result<void> AppleAscMailboxV4::send(uint32_t channel, const void *data, size_t size) {
	assert(channel == 0);
	assert(size == sizeof(AppleMailboxMessage));

	AppleMailboxMessage message;
	memcpy(&message, data, sizeof(AppleMailboxMessage));

	_mmio.store(apple_asc_v4::a2iMsg0, message.msg0);
	_mmio.store(apple_asc_v4::a2iMsg1, apple_asc_v4::msg1::msg(message.msg1));

	// Wait until the message is consumed
	while (_mmio.load(apple_asc_v4::a2iStatus) & apple_asc_v4::status::full) {
		co_await helix::sleepFor(1'000'000);
	}

	co_return;
}

async::result<void> AppleAscMailboxV4::receive(uint32_t channel, void *data, size_t size) {
	assert(channel == 0);
	assert(size == sizeof(AppleMailboxMessage));

	// Wait until a message is available
	while (_mmio.load(apple_asc_v4::i2aStatus) & apple_asc_v4::status::empty) {
		co_await helix::sleepFor(1'000'000);
	}

	auto msg0 = _mmio.load(apple_asc_v4::i2aMsg0);
	auto msg1 = _mmio.load(apple_asc_v4::i2aMsg1);

	AppleMailboxMessage msg;
	msg.msg0 = msg0;
	msg.msg1 = msg1 & apple_asc_v4::msg1::msg;

	memcpy(data, &msg, sizeof(AppleMailboxMessage));
	co_return;
}

async::detached AppleAscMailboxV4::_handleIrqs() {
	co_await _

	    while (true) {
		auto a2iStatus = _mmio.load(apple_asc_v4::a2iStatus);
		auto i2aStatus = _mmio.load(apple_asc_v4::i2aStatus);

		if (!(a2iStatus & apple_asc_v4::status::empty)) {
			std::println("apple-asc {}: ASC mailbox A2I not empty", _location);
		}
		if (!(i2aStatus & apple_asc_v4::status::empty)) {
			std::println("apple-asc {}: ASC mailbox I2A not empty", _location);

			AppleMailboxMessage msg;
			receive(0, &msg, sizeof(msg));
			std::println(
			    "apple-asc {}: Received message: msg0={:#x}, msg1={:#x}",
			    _location,
			    msg.msg0,
			    msg.msg1
			);
		}

		co_await helix::sleepFor(100'000'000);
	}

	size_t sequence = 0;

	while (true) {
		auto irq = co_await helix_ng::awaitEvent(_recvNotEmptyIrq, sequence);
		HEL_CHECK(irq.error());
		sequence = irq.sequence();

		std::println("apple-asc {}: ASC mailbox IRQ received, sequence={}", _location, sequence);

		// ...

		HEL_CHECK(helAcknowledgeIrq(_recvNotEmptyIrq.getHandle(), kHelAckAcknowledge, sequence));
	}
}

} // namespace mailboxd
