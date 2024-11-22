#include <arch/bits.hpp>
#include <arch/io_space.hpp>
#include <arch/register.hpp>
#include <thor-internal/arch/debug.hpp>

namespace thor {

constinit PIOLogHandler pioLogHandler;

inline constexpr arch::scalar_register<uint8_t> data(0);
inline constexpr arch::scalar_register<uint8_t> baudLow(0);
inline constexpr arch::scalar_register<uint8_t> interruptEnable(1);
inline constexpr arch::scalar_register<uint8_t> baudHigh(1);
inline constexpr arch::bit_register<uint8_t> fifoControl(2);
inline constexpr arch::bit_register<uint8_t> lineControl(3);
inline constexpr arch::bit_register<uint8_t> modemControl(4);
inline constexpr arch::bit_register<uint8_t> lineStatus(5);

inline constexpr arch::field<uint8_t, bool> txReady(5, 1);

inline constexpr arch::field<uint8_t, int> dataBits(0, 2);
inline constexpr arch::field<uint8_t, bool> stopBit(2, 1);
inline constexpr arch::field<uint8_t, int> parityBits(3, 3);
inline constexpr arch::field<uint8_t, bool> dlab(7, 1);

inline constexpr arch::field<uint8_t, bool> enableFifos(0, 1);
inline constexpr arch::field<uint8_t, bool> clearRxFifo(1, 1);
inline constexpr arch::field<uint8_t, bool> clearTxFifo(2, 1);

inline constexpr arch::field<uint8_t, bool> dtr(0, 1);
inline constexpr arch::field<uint8_t, bool> rts(1, 1);

extern bool debugToSerial;
extern bool debugToBochs;

void setupDebugging() {
	if(debugToSerial) {
		auto base = arch::global_io.subspace(0x3F8);

		// disable all interrupts
		base.store(interruptEnable, 0);

		// Set the baud rate.
		base.store(lineControl, dlab(true));
		base.store(baudLow, 0x01);
		base.store(baudHigh, 0x00);

		// Configure: 8 data bits, 1 stop bit, no parity.
		base.store(lineControl, dataBits(3) | stopBit(0) | parityBits(0) | dlab(false));

		// clear and enable FIFOs
		base.store(fifoControl, enableFifos(true) | clearRxFifo(true) | clearTxFifo(true));

		// set DTR + RTS
		base.store(modemControl, dtr(true) | rts(true));
	}

	enableLogHandler(&pioLogHandler);
}

void PIOLogHandler::emit(frg::string_view record) {
	auto [md, msg] = destructureLogRecord(record);
	setPriority(md.severity);
	for (size_t i = 0; i < msg.size(); ++i)
		printChar(msg[i]);
	resetPriority();
	printChar('\n');
}

void PIOLogHandler::emitUrgent(frg::string_view record) {
	auto [md, msg] = destructureLogRecord(record);
	setPriority(md.severity);
	const char *prefix = "URGENT: ";
	while(*prefix)
		printChar(*(prefix++));
	for (size_t i = 0; i < msg.size(); ++i)
		printChar(msg[i]);
	resetPriority();
	printChar('\n');
}

void PIOLogHandler::printChar(char c) {
	auto sendByteSerial = [this](uint8_t val) {
		auto base = arch::global_io.subspace(0x3F8);

		serialBuffer[serialBufferIndex++] = val;
		if (serialBufferIndex == 16) {
			while(!(base.load(lineStatus) & txReady)) {
				// do nothing until the UART is ready to transmit.
			}
			base.store_iterative(data, serialBuffer, 16);
			serialBufferIndex = 0;
		}
	};

	if(debugToSerial) {
		if(c == '\n') {
			sendByteSerial('\r');
		}

		sendByteSerial(c);
	}

	if(debugToBochs) {
		auto base = arch::global_io.subspace(0xE9);
		base.store(data, c);
	}
}

void PIOLogHandler::setPriority(Severity prio) {
	int c = 9;

	switch(prio) {
		case Severity::emergency:
		case Severity::alert:
		case Severity::critical:
		case Severity::error:
			c = 1;
			break;
		case Severity::warning:
			c = 3;
			break;
		case Severity::notice:
		case Severity::info:
			c = 9;
			break;
		case Severity::debug:
			c = 5;
			break;
	}

	printChar('\e');
	printChar('[');
	printChar('3');
	printChar('0' + c);
	printChar('m');
}

void PIOLogHandler::resetPriority() {
	printChar('\e');
	printChar('[');
	printChar('3');
	printChar('9');
	printChar('m');
}

} // namespace thor
