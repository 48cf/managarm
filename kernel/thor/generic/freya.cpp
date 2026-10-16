#include <thor-internal/address-space.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/credentials.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/elf-loader.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/freya.hpp>
#include <thor-internal/hierarchy.hpp>
#include <thor-internal/load-balancing.hpp>
#include <thor-internal/module.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/universe.hpp>

namespace thor {

extern MfsDirectory *mfsRoot;

namespace {

constexpr size_t freyaStackSize = 0x10000;

// Path of the freya image inside the initrd. freya is launched directly rather
// than through svrctl, so it has no server description and no .bin file.
constexpr const char *freyaPath = "usr/bin/freya";

} // anonymous namespace

coroutine<void> runFreya(smarter::shared_ptr<MemoryView> initrdView, size_t initrdOffset,
		size_t initrdSize, smarter::shared_ptr<Stream, LanePolicy> mbusServerLane) {
	auto module = resolveModule(freyaPath);
	if(!module)
		panicLogger() << "thor: Could not find " << freyaPath << " in the initrd" << frg::endlog;
	assert(module->type == MfsType::regular);

	auto hierarchyOutcome = Hierarchy::extend(rootHierarchy(),
			frg::string<KernelAlloc>{*kernelAlloc, "freya"});
	if(!hierarchyOutcome)
		panicLogger() << "thor: Failed to extend hierarchy for freya" << frg::endlog;
	auto hierarchy = std::move(*hierarchyOutcome);

	auto spaceOutcome = AddressSpace::create();
	if(!spaceOutcome)
		panicLogger() << "thor: Failed to create address space" << frg::endlog;
	auto space = std::move(*spaceOutcome);

	auto image = co_await loadModuleImage(hierarchy, space, 0,
			static_cast<MfsRegular *>(module)->getMemory());
	// freya has to be statically linked: it provides the supercalls that mlibc's
	// startup path issues, so anything that ran rtdl first would deadlock against
	// the service freya is bringing up. There is also no rtdl to run it with --
	// nothing sets up an aux vector for freya.
	if(!image.interpreter.empty())
		panicLogger() << "thor: " << freyaPath << " requests an interpreter,"
				" but it must be statically linked" << frg::endlog;

	auto stackMemoryOutcome = AllocatedMemory::create(hierarchy, freyaStackSize);
	if(!stackMemoryOutcome)
		panicLogger() << "thor: Failed to create memory" << frg::endlog;
	auto stackViewOutcome = MemorySlice::create(std::move(*stackMemoryOutcome), 0, freyaStackSize);
	if(!stackViewOutcome)
		panicLogger() << "thor: Failed to create memory slice" << frg::endlog;

	auto stackMapResult = co_await space->map(std::move(*stackViewOutcome), 0, 0, freyaStackSize,
			AddressSpace::kMapPreferTop | AddressSpace::kMapProtRead
				| AddressSpace::kMapProtWrite);
	assert(stackMapResult);

	auto universeOutcome = Universe::create();
	if(!universeOutcome)
		panicLogger() << "thor: Failed to create universe" << frg::endlog;
	auto universe = std::move(*universeOutcome);

	HelBootstrapData bootstrap{};
	bootstrap.initrdOffset = initrdOffset;
	bootstrap.initrdSize = initrdSize;
	bootstrap.initrdMemory = universe->attachDescriptor(
		AnyDescriptor::make<DescriptorType::memoryView>(std::move(initrdView),
				kHelRightRead | kHelRightAssign)
	);
	if(mbusServerLane) {
		bootstrap.mbusServerLane = universe->attachDescriptor(
			AnyDescriptor::make<DescriptorType::lane>(std::move(mbusServerLane), kHelRightInvoke)
		);
	}
	// freya grants this on to the servers whose description asks for
	// 'access:thor.hardware', the way runService() does today. kHelRightInvoke is
	// all it needs to do so: helTransferDescriptor() checks kHelRightGrant on the
	// target universe rather than on the descriptor being transferred, and it can
	// only narrow the rights it carries over.
	bootstrap.hardwareAccess = universe->attachDescriptor(
		AnyDescriptor::make<DescriptorType::token>(hardwareAccessToken(), kHelRightInvoke)
	);

	bootstrap.hierarchy = universe->attachDescriptor(
		AnyDescriptor::make<DescriptorType::hierarchy>(hierarchy,
				kHelRightDerive | kHelRightProvision)
	);

	// freya gets no aux vector, so the handoff lives in a page of its own and is
	// reached through the argument register.
	static_assert(sizeof(HelBootstrapData) <= kPageSize);
	auto bootstrapMemoryOutcome = AllocatedMemory::create(hierarchy, kPageSize);
	if(!bootstrapMemoryOutcome)
		panicLogger() << "thor: Failed to create memory" << frg::endlog;
	auto bootstrapMemory = std::move(*bootstrapMemoryOutcome);
	auto copyBootstrapOutcome = co_await bootstrapMemory->copyTo(0, &bootstrap,
			sizeof(HelBootstrapData));
	assert(copyBootstrapOutcome);

	auto bootstrapViewOutcome = MemorySlice::create(std::move(bootstrapMemory), 0, kPageSize);
	if(!bootstrapViewOutcome)
		panicLogger() << "thor: Failed to create memory slice" << frg::endlog;
	auto bootstrapMapResult = co_await space->map(std::move(*bootstrapViewOutcome), 0, 0, kPageSize,
			AddressSpace::kMapPreferTop | AddressSpace::kMapProtRead);
	assert(bootstrapMapResult);

	AbiParameters params;
	params.ip = reinterpret_cast<uintptr_t>(image.entryIp);
	// The stack is empty: freya's _start expects nothing on it, and the ABI
	// alignment it does expect follows from the mapping being page aligned.
	params.sp = stackMapResult.value() + freyaStackSize;
	params.argument = bootstrapMapResult.value();

	auto threadOutcome = Thread::create(std::move(universe), std::move(space), params);
	if(!threadOutcome)
		panicLogger() << "thor: Failed to create thread" << frg::endlog;
	auto thread = std::move(*threadOutcome);

	// see helCreateThread for the reasoning here
	thread.policy().increment();
	thread.policy().increment();

	LoadBalancer::singleton().connect(thread.get(), getCpuData());
	Scheduler::associate(thread.get(), &localScheduler.get());
	Scheduler::resume(thread.get());
	Thread::resumeOther(smarter::rc_policy_downcast<smarter::default_rc_policy>(thread));
}

} // namespace thor
