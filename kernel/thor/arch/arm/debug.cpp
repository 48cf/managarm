#include <arch/aarch64/mem_space.hpp>
#include <arch/register.hpp>
#include <eir/interface.hpp>
#include <thor-internal/arch/debug.hpp>
#include <thor-internal/elf-notes.hpp>

namespace {

namespace pl011 {

namespace regs {

static constexpr arch::scalar_register<uint32_t> data{0x00};
static constexpr arch::bit_register<uint32_t> status{0x18};

} // namespace regs

namespace status {

static constexpr arch::field<uint32_t, bool> txFull{5, 1};

} // namespace status

} // namespace pl011

namespace s5l {

namespace regs {

static constexpr arch::bit_register<uint32_t> status{0x10};
static constexpr arch::scalar_register<uint32_t> data{0x20};

} // namespace regs

namespace status {

static constexpr arch::field<uint32_t, bool> txEmpty{1, 1};

} // namespace status

} // namespace s5l

} // namespace

namespace thor {

constinit UartLogHandler uartLogHandler;

extern bool debugToSerial;

void setupDebugging() {
	if (debugToSerial)
		enableLogHandler(&uartLogHandler);
}

extern ManagarmElfNote<BootUartConfig> bootUartConfig;
THOR_DEFINE_ELF_NOTE(bootUartConfig){elf_note_type::bootUartConfig, {}};

void UartLogHandler::emit(frg::string_view record) {
	auto [md, msg] = destructureLogRecord(record);
	for (size_t i = 0; i < msg.size(); ++i)
		printChar(msg[i]);
	printChar('\n');
}

void UartLogHandler::emitUrgent(frg::string_view record) {
	auto [md, msg] = destructureLogRecord(record);
	const char *prefix = "URGENT: ";
	while (*prefix)
		printChar(*(prefix++));
	for (size_t i = 0; i < msg.size(); ++i)
		printChar(msg[i]);
	printChar('\n');
}

void UartLogHandler::printChar(char c) {
	if (bootUartConfig->type == BootUartType::none) {
		return;
	}

	// We assume here that Eir has mapped the UART as device memory, and
	// configured the UART to some sensible settings (115200 8N1).
	arch::mem_space space{bootUartConfig->address};

	if (bootUartConfig->type == BootUartType::pl011) {
		while (space.load(pl011::regs::status) & pl011::status::txFull) {
		}

		space.store(pl011::regs::data, c);
	} else if (bootUartConfig->type == BootUartType::s5l) {
		while (!(space.load(s5l::regs::status) & s5l::status::txEmpty)) {
		}

		space.store(s5l::regs::data, c);
	}
}

} // namespace thor
