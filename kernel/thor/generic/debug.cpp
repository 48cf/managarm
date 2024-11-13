#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/arch/stack.hpp>

namespace thor {

namespace {
	// Protects the data structures below.
	constinit frg::ticket_spinlock logMutex;

	frg::manual_box<frg::intrusive_list<
		LogHandler,
		frg::locate_member<
			LogHandler,
			frg::default_list_hook<LogHandler>,
			&LogHandler::hook
		>
	>> globalLogList;
} // anonymous namespace

void enableLogHandler(LogHandler *sink) {
	if (!globalLogList)
		globalLogList.initialize();

	globalLogList->push_back(sink);
}

void disableLogHandler(LogHandler *sink) {
	if (!globalLogList)
		globalLogList.initialize();

	auto it = globalLogList->iterator_to(sink);
	globalLogList->erase(it);
}

namespace {
	void emitLog(Severity severity, frg::string_view msg) {
		for (const auto &it : *globalLogList)
			it->emit(severity, msg);
	}

	// This class splits long log messages into lines.
	// In also ensures that we never emit partial CSI sequences.
	class LogProcessor {
	public:
		void print(char c) {
			auto doesFit = [&] (int n) -> bool {
				return stagedLength + n < logLineLength;
			};

			auto emit = [&] (char c) {
				stagingBuffer[stagedLength++] = c;
			};

			auto flush = [&] () {
				emitLog(severity, frg::string_view{stagingBuffer, stagedLength});

				// Reset our staging buffer.
				memset(stagingBuffer, 0, logLineLength);
				stagedLength = 0;
			};

			if(!csiState) {
				if(c == '\x1B') {
					csiState = 1;
				}else if(c == '\n') {
					flush();
				}else{
					if(!doesFit(1))
						flush();

					assert(doesFit(1));
					emit(c);
				}
			}else if(csiState == 1) {
				if(c == '[') {
					csiState = 2;
				}else{
					if(!doesFit(2))
						flush();

					assert(doesFit(2));
					emit('\x1B');
					emit(c);
					csiState = 0;
				}
			}else{
				// This is csiState == 2.
				if((c >= '0' && c <= '9') || (c == ';')) {
					if(csiLength < maximalCsiLength)
						csiBuffer[csiLength] = c;
					csiLength++;
				}else{
					if(csiLength >= maximalCsiLength || !doesFit(3 + csiLength))
						flush();

					assert(doesFit(3 + csiLength));
					emit('\x1B');
					emit('[');
					for(int i = 0; i < csiLength; i++)
						emit(csiBuffer[i]);
					emit(c);
					csiState = 0;
					csiLength = 0;
				}
			}
		}

		void print(const char *str) {
			while(*str)
				print(*str++);
		}

		void setPriority(Severity prio) {
			severity = prio;
		}

	private:
		static constexpr int maximalCsiLength = 16;

		Severity severity{};

		char csiBuffer[maximalCsiLength]{};
		int csiState = 0;
		int csiLength = 0;

		char stagingBuffer[logLineLength]{};
		size_t stagedLength = 0;
	};

	constinit LogProcessor logProcessor;
} // anonymous namespace

void panic() {
	disableInts();
	while(true)
		halt();
}

constinit frg::stack_buffer_logger<DebugSink, logLineLength> debugLogger;
constinit frg::stack_buffer_logger<WarningSink, logLineLength> warningLogger;
constinit frg::stack_buffer_logger<InfoSink, logLineLength> infoLogger;
constinit frg::stack_buffer_logger<UrgentSink, logLineLength> urgentLogger;
constinit frg::stack_buffer_logger<PanicSink, logLineLength> panicLogger;

void DebugSink::operator() (const char *msg) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&logMutex);

	logProcessor.setPriority(Severity::debug);
	logProcessor.print(msg);
	logProcessor.print('\n'); // Note: this is also required to flush.
}

void WarningSink::operator() (const char *msg) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&logMutex);

	logProcessor.setPriority(Severity::warning);
	logProcessor.print(msg);
	logProcessor.print('\n'); // Note: this is also required to flush.
}

void InfoSink::operator() (const char *msg) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&logMutex);

	logProcessor.setPriority(Severity::info);
	logProcessor.print(msg);
	logProcessor.print('\n'); // Note: this is also required to flush.
}

void UrgentSink::operator() (const char *msg) {
	StatelessIrqLock irqLock;
	auto lock = frg::guard(&logMutex);

	logProcessor.setPriority(Severity::critical);
	logProcessor.print(msg);
	logProcessor.print('\n'); // Note: this is also required to flush.
}

void PanicSink::operator() (const char *msg) {
	StatelessIrqLock irqLock;

	auto lock = frg::guard(&logMutex);

	logProcessor.setPriority(Severity::emergency);
	logProcessor.print(msg);
	logProcessor.print('\n'); // Note: this is also required to flush.
}

void PanicSink::finalize(bool) {
	StatelessIrqLock irqLock;

#ifdef THOR_HAS_FRAME_POINTERS
	urgentLogger() << "Stacktrace:" << frg::endlog;
	walkThisStack([&](uintptr_t ip) {
		urgentLogger() << "\t<" << (void*)ip << ">" << frg::endlog;
	});
#endif

	panic();
}

} // namespace thor

extern "C" void __assert_fail(const char *assertion, const char *file,
		unsigned int line, const char *function) {
	thor::panicLogger() << "Assertion failed: " << assertion << "\n"
			<< "In function " << function
			<< " at " << file << ":" << line << frg::endlog;
}

extern "C" void __cxa_pure_virtual() {
	thor::panicLogger() << "Pure virtual call" << frg::endlog;
}
