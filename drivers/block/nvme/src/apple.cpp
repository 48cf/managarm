#include <print>

#include <helix/timer.hpp>

#include "apple.hpp"

#define BIT(n) (UINT64_C(1) << (n))

#define NVME_TIMEOUT 1'000'000'000
#define NVME_ENABLE_TIMEOUT 5'000'000'000
#define NVME_SHUTDOWN_TIMEOUT 5000000
#define NVME_QUEUE_SIZE 64

#define NVME_CC 0x14
// #define NVME_CC_SHN GENMASK(15, 14)
#define NVME_CC_SHN_NONE 0
#define NVME_CC_SHN_NORMAL 1
#define NVME_CC_SHN_ABRUPT 2
#define NVME_CC_EN BIT(0)

#define NVME_CSTS 0x1c
// #define NVME_CSTS_SHST GENMASK(3, 2)
#define NVME_CSTS_SHST_NORMAL 0
#define NVME_CSTS_SHST_BUSY 1
#define NVME_CSTS_SHST_DONE 2
#define NVME_CSTS_RDY BIT(0)

#define NVME_AQA 0x24
#define NVME_ASQ 0x28
#define NVME_ACQ 0x30

#define NVME_DB_ACQ 0x1004
#define NVME_DB_IOCQ 0x100c

#define NVME_BOOT_STATUS 0x1300
#define NVME_BOOT_STATUS_OK 0xde71ce55

#define NVME_LINEAR_SQ_CTRL 0x24908
#define NVME_LINEAR_SQ_CTRL_EN BIT(0)

#define NVME_UNKNOWN_CTRL 0x24008
#define NVME_UNKNOWN_CTRL_PRP_NULL_CHECK BIT(11)

#define NVME_MAX_PEND_CMDS_CTRL 0x1210
#define NVME_DB_LINEAR_ASQ 0x2490c
#define NVME_DB_LINEAR_IOSQ 0x24910

#define NVMMU_NUM 0x28100
#define NVMMU_ASQ_BASE 0x28108
#define NVMMU_IOSQ_BASE 0x28110
#define NVMMU_TCB_INVAL 0x28118
#define NVMMU_TCB_STAT 0x29120

#define NVME_ADMIN_CMD_DELETE_SQ 0x00
#define NVME_ADMIN_CMD_CREATE_SQ 0x01
#define NVME_ADMIN_CMD_DELETE_CQ 0x04
#define NVME_ADMIN_CMD_CREATE_CQ 0x05
#define NVME_QUEUE_CONTIGUOUS BIT(0)
#define NVME_CQ_IRQ_EN BIT(1)

#define NVME_CMD_FLUSH 0x00
#define NVME_CMD_WRITE 0x01
#define NVME_CMD_READ 0x02

#define ANS_MODESEL 0x01304
#define ANS_NVMMU_NUM 0x28100
#define ANS_NVMMU_TCB_SIZE 0x4000
#define ANS_NVMMU_TCB_PITCH 0x80

#define APPLE_ANS_TCB_DMA_FROM_DEVICE BIT(0)
#define APPLE_ANS_TCB_DMA_TO_DEVICE BIT(1)

namespace nvme_regs {

arch::scalar_register<uint32_t> bootStatus{0x1300};

} // namespace nvme_regs

namespace ans_regs {

arch::bit_register<uint32_t> cpuControl{0x44};

namespace cpu_control {

constexpr arch::field<uint32_t, bool> run{4, 1};

} // namespace cpu_control

} // namespace ans_regs

namespace nvme {

AppleAns2NvmeController::AppleAns2NvmeController(
    int64_t parentId,
    std::string location,
    std::unique_ptr<apple::Sart> sart,
    std::unique_ptr<apple::RtKit> rtkit,
    helix::Mapping nvmeMapping,
    helix::Mapping ansMapping
)
: Controller{parentId, std::move(location), ControllerType::AppleAns2},
  _nvmeMapping{std::move(nvmeMapping)},
  _ansMapping{std::move(ansMapping)},
  _nvmeMmio{_nvmeMapping.get()},
  _ansMmio{_ansMapping.get()},
  _sart{std::move(sart)},
  _rtkit{std::move(rtkit)} {
	version_ = arch::scalar_load<uint32_t>(_nvmeMmio, 0x0008);
	std::println("apple-ans2 {}: NVMe version {:#x}", location_, version_);
}

async::detached AppleAns2NvmeController::run(mbus_ng::EntityId subsystem) {
	std::println(
	    "apple-ans2 {}: Starting controller, boot_status={:#x}",
	    location_,
	    _nvmeMmio.load(nvme_regs::bootStatus)
	);

	if (_ansMmio.load(ans_regs::cpuControl) & ans_regs::cpu_control::run) {
		std::println("apple-ans2 {}: Controller is running", location_);
	} else {
		std::println("apple-ans2 {}: Controller is not running", location_);

		_ansMmio.store(ans_regs::cpuControl, ans_regs::cpu_control::run(true));

		co_await _rtkit->boot();
	}

	auto booted = co_await helix::kindaBusyWait(1'000'000'000, [&]() {
		return _nvmeMmio.load(nvme_regs::bootStatus) == 0xde71ce55;
	});

	if (!booted) {
		std::println(
		    "apple-ans2 {}: ANS failed to boot, boot_status={:#x}",
		    location_,
		    _nvmeMmio.load(nvme_regs::bootStatus)
		);
		co_return;
	}

	std::println("apple-ans2 {}: ANS booted successfully", location_);

	uint32_t linearSqCtrl = NVME_LINEAR_SQ_CTRL_EN;
	arch::scalar_store<uint32_t>(_nvmeMmio, NVME_LINEAR_SQ_CTRL, NVME_LINEAR_SQ_CTRL_EN);

	uint32_t pendCmdsCtrl = (NVME_QUEUE_SIZE << 16) | NVME_QUEUE_SIZE;
	arch::scalar_store<uint32_t>(_nvmeMmio, NVME_MAX_PEND_CMDS_CTRL, pendCmdsCtrl);

	auto nvmmuNumTcbs = NVME_QUEUE_SIZE - 1;
	arch::scalar_store<uint32_t>(_nvmeMmio, NVMMU_NUM, nvmmuNumTcbs);

	auto unknownCtrl = arch::scalar_load<uint32_t>(_nvmeMmio, NVME_UNKNOWN_CTRL);
	unknownCtrl &= ~NVME_UNKNOWN_CTRL_PRP_NULL_CHECK;
	arch::scalar_store<uint32_t>(_nvmeMmio, NVME_UNKNOWN_CTRL, unknownCtrl);

	// uint32_t nvmmuNum = (ANS_NVMMU_TCB_SIZE / ANS_NVMMU_TCB_PITCH) - 1;
	// arch::scalar_store<uint32_t>(_nvmeMmio, NVMMU_NUM, nvmmuNum);
	// arch::scalar_store<uint32_t>(_nvmeMmio, ANS_MODESEL, 0);

	auto adminQueue = std::make_unique<AppleAns2NvmeQueue>(this, QueueType::admin);
	co_await adminQueue->init();
	co_await _enable();
	adminQueue->run();
	activeQueues_.push_back(std::move(adminQueue));

	co_await helix::sleepFor(2'500'000'000);

	auto ioQueue = std::make_unique<AppleAns2NvmeQueue>(this, QueueType::io);
	co_await ioQueue->init();
	ioQueue->run();
	activeQueues_.push_back(std::move(ioQueue));

	co_await helix::sleepFor(1'000'000'000);
	co_await scanNamespaces();

	mbus_ng::Properties descriptor{
	    {"class", mbus_ng::StringItem{"nvme-controller"}},
	    {"nvme.subsystem", mbus_ng::StringItem{std::to_string(subsystem)}},
	    {"nvme.address", mbus_ng::StringItem{location_}},
	    {"nvme.transport", mbus_ng::StringItem{"mmio"}},
	    {"nvme.serial", mbus_ng::StringItem{serial}},
	    {"nvme.model", mbus_ng::StringItem{model}},
	    {"nvme.fw-rev", mbus_ng::StringItem{fw_rev}},
	    {"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(parentId_)}},
	};

	mbusEntity_ = std::make_unique<mbus_ng::EntityManager>(
	    (co_await mbus_ng::Instance::global().createEntity("nvme-controller", descriptor)).unwrap()
	);

	for (auto &ns : activeNamespaces_) {
		ns->run();
	}
}

async::result<Command::Result>
AppleAns2NvmeController::submitAdminCommand(std::unique_ptr<Command> cmd) {
	auto result = co_await activeQueues_.front()->submitCommand(std::move(cmd));
	co_return result;
}

async::result<Command::Result> AppleAns2NvmeController::submitIoCommand(std::unique_ptr<Command> cmd
) {
	auto result = co_await activeQueues_.back()->submitCommand(std::move(cmd));
	co_return result;
}

async::result<void> AppleAns2NvmeController::_disable() {
	auto cc = arch::scalar_load<uint32_t>(_nvmeMmio, NVME_CC);
	cc &= ~NVME_CC_EN;
	arch::scalar_store<uint32_t>(_nvmeMmio, NVME_CC, cc);

	auto disabled = co_await helix::kindaBusyWait(NVME_ENABLE_TIMEOUT, [&]() {
		auto csts = arch::scalar_load<uint32_t>(_nvmeMmio, NVME_CSTS);
		return !(csts & NVME_CSTS_RDY);
	});

	if (!disabled) {
		std::println("apple-ans2 {}: Timed out waiting for controller to disable", location_);
		assert(false);
	}

	std::println("apple-ans2 {}: Controller disabled", location_);
}

async::result<void> AppleAns2NvmeController::_enable() {
	auto cc = arch::scalar_load<uint32_t>(_nvmeMmio, NVME_CC);
	cc |= NVME_CC_EN;
	arch::scalar_store<uint32_t>(_nvmeMmio, NVME_CC, cc);

	auto enabled = co_await helix::kindaBusyWait(NVME_ENABLE_TIMEOUT, [&]() {
		auto csts = arch::scalar_load<uint32_t>(_nvmeMmio, NVME_CSTS);
		return (csts & NVME_CSTS_RDY);
	});

	if (!enabled) {
		std::println("apple-ans2 {}: Timed out waiting for controller to enable", location_);
		assert(false);
	}

	std::println("apple-ans2 {}: Controller enabled", location_);
}

AppleAns2NvmeQueue::AppleAns2NvmeQueue(AppleAns2NvmeController *ctrl, QueueType queueType)
: Queue{0, NVME_QUEUE_SIZE},
  _queueType{queueType},
  _ctrl{ctrl},
  _head{0},
  _phase{1} {
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

	auto tcbBuffer = allocateWithAlignment(sizeof(NvmmuTcb) * depth_, 0x4000);
	auto cmdBuffer = allocateWithAlignment(sizeof(spec::Command) * depth_, 0x4000);
	auto completionsBuffer = allocateWithAlignment(sizeof(spec::CompletionEntry) * depth_, 0x4000);

	_tcbs = (NvmmuTcb *)tcbBuffer;
	_cmds = (spec::Command *)cmdBuffer;
	_completions = (spec::CompletionEntry *)completionsBuffer;

	memset(_tcbs, 0, sizeof(NvmmuTcb) * depth_);
	memset(_cmds, 0, sizeof(spec::Command) * depth_);
	memset(_completions, 0, sizeof(spec::CompletionEntry) * depth_);

	_barrier.writeback(_tcbs, sizeof(NvmmuTcb) * depth_);
	_barrier.writeback(_cmds, sizeof(spec::Command) * depth_);
	_barrier.writeback(_completions, sizeof(spec::CompletionEntry) * depth_);

	asm volatile("dsb osh" ::: "memory");
}

async::result<void> AppleAns2NvmeQueue::init() {
	uint64_t tcbs = helix::ptrToPhysical(_tcbs);
	uint64_t cmds = helix::ptrToPhysical(_cmds);
	uint64_t completions = helix::ptrToPhysical(_completions);

	if (_queueType == QueueType::admin) {
		arch::scalar_store<uint32_t>(
		    _ctrl->_nvmeMmio, NVME_AQA, ((depth_ - 1) << 16) | (depth_ - 1)
		);

		arch::scalar_store<uint32_t>(_ctrl->_nvmeMmio, NVME_ASQ, cmds & 0xffffffff);
		arch::scalar_store<uint32_t>(_ctrl->_nvmeMmio, NVME_ASQ + 4, cmds >> 32);

		arch::scalar_store<uint32_t>(_ctrl->_nvmeMmio, NVME_ACQ, completions & 0xffffffff);
		arch::scalar_store<uint32_t>(_ctrl->_nvmeMmio, NVME_ACQ + 4, completions >> 32);

		arch::scalar_store<uint32_t>(_ctrl->_nvmeMmio, NVMMU_ASQ_BASE, tcbs & 0xffffffff);
		arch::scalar_store<uint32_t>(_ctrl->_nvmeMmio, NVMMU_ASQ_BASE + 4, tcbs >> 32);
	} else {
		arch::scalar_store<uint32_t>(_ctrl->_nvmeMmio, NVMMU_IOSQ_BASE, tcbs & 0xffffffff);
		arch::scalar_store<uint32_t>(_ctrl->_nvmeMmio, NVMMU_IOSQ_BASE + 4, tcbs >> 32);

		{
			auto cmd = std::make_unique<Command>();
			// auto spec = &cmd->getCommandBuffer().createCQ;
			auto spec = &cmd->getCommandBuffer().common;

			// spec->opcode = NVME_ADMIN_CMD_CREATE_CQ;
			// spec->prp1 = completions;
			// spec->cqid = 1;
			// spec->qSize = depth_ - 1;
			// spec->cqFlags = NVME_QUEUE_CONTIGUOUS;
			// spec->irqVector = 0;
			spec->opcode = static_cast<uint8_t>(spec::AdminOpcode::CreateCQ);
			spec->dataPtr.prp.prp1 = completions;
			spec->cdw10 = (depth_ - 1) << 16 | 1;
			spec->cdw11 = NVME_QUEUE_CONTIGUOUS | NVME_CQ_IRQ_EN;

			co_await _ctrl->submitAdminCommand(std::move(cmd));
		}

		{
			auto cmd = std::make_unique<Command>();
			// auto spec = &cmd->getCommandBuffer().createSQ;
			auto spec = &cmd->getCommandBuffer().common;

			// spec->opcode = NVME_ADMIN_CMD_CREATE_SQ;
			// spec->prp1 = cmds;
			// spec->sqid = 1;
			// spec->qSize = depth_ - 1;
			// spec->sqFlags = NVME_QUEUE_CONTIGUOUS;
			// spec->cqid = 1;
			spec->opcode = static_cast<uint8_t>(spec::AdminOpcode::CreateSQ);
			spec->dataPtr.prp.prp1 = cmds;
			spec->cdw10 = (depth_ - 1) << 16 | 1;
			spec->cdw11 = (1 << 16) | NVME_QUEUE_CONTIGUOUS;

			co_await _ctrl->submitAdminCommand(std::move(cmd));
		}
	}
}

async::detached AppleAns2NvmeQueue::run() {
	while (true) {
		auto cmd = co_await pendingCmdQueue_.async_get();
		if (!cmd) {
			continue;
		}

		asm volatile("dsb osh" ::: "memory");
		auto command = std::move(cmd.value());
		co_await submitCommandToDevice(std::move(command));
	}
}

async::result<Command::Result> AppleAns2NvmeQueue::submitCommand(std::unique_ptr<Command> cmd) {
	auto future = cmd->getFuture();
	pendingCmdQueue_.put(std::move(cmd));
	co_return *(co_await future.get());
}

async::result<void> AppleAns2NvmeQueue::submitCommandToDevice(std::unique_ptr<Command> command) {
	auto slot = co_await findFreeSlot();
	auto spec = &command->getCommandBuffer().common;

	auto queueCmd = &_cmds[slot];
	auto queueTcb = &_tcbs[slot];
	auto queueCompletion = &_completions[slot];

	memset(queueTcb, 0, sizeof(NvmmuTcb));
	memcpy(queueCmd, spec, sizeof(spec::Command));

	queueTcb->opcode = queueCmd->common.opcode;
	queueTcb->slot = slot;
	queueTcb->len = queueCmd->common.cdw12;
	queueTcb->prp1 = queueCmd->common.dataPtr.prp.prp1;
	queueTcb->prp2 = queueCmd->common.dataPtr.prp.prp2;

	// If the command is a write (i.e. to device), set the appropriate flag.
	if ((queueCmd->common.opcode & 1) != 0) {
		queueTcb->flags |= APPLE_ANS_TCB_DMA_TO_DEVICE;
	} else {
		queueTcb->flags |= APPLE_ANS_TCB_DMA_FROM_DEVICE;
	}

	asm volatile("dsb osh" ::: "memory");

	_barrier.writeback(queueCmd, sizeof(spec::Command));
	_barrier.writeback(queueTcb, sizeof(NvmmuTcb));

	{
		co_await _mutex.async_lock();
		frg::unique_lock consistencyLock{frg::adopt_lock, _mutex};

		queuedCmds_[slot] = std::move(command);
		commandsInFlight_++;

		std::println(
		    "apple-ans2 {}: Submitting command to device on slot {}", _ctrl->location_, slot
		);

		if (_queueType == QueueType::admin) {
			arch::scalar_store<uint32_t>(_ctrl->_nvmeMmio, NVME_DB_LINEAR_ASQ, slot);
		} else {
			arch::scalar_store<uint32_t>(_ctrl->_nvmeMmio, NVME_DB_LINEAR_IOSQ, slot);
		}
	}

	spec::CompletionEntry completion;
	while (true) {
		asm volatile("dsb osh" ::: "memory");
		_barrier.invalidate(queueCompletion, sizeof(spec::CompletionEntry));

		memcpy(&completion, queueCompletion, sizeof(spec::CompletionEntry));

		if ((completion.status.status & 1) != _phase) {
			co_await helix::sleepFor(5'000'000);
			continue;
		}

		{
			co_await _mutex.async_lock();
			frg::unique_lock consistencyLock{frg::adopt_lock, _mutex};

			asm volatile("dsb osh" ::: "memory");
			auto command = std::move(queuedCmds_[slot]);
			command->complete(completion.status, completion.result);
			commandsInFlight_--;
			freeSlotDoorbell_.raise();

			arch::scalar_store<uint32_t>(_ctrl->_nvmeMmio, NVMMU_TCB_INVAL, slot);

			if (arch::scalar_load<uint32_t>(_ctrl->_nvmeMmio, NVMMU_TCB_STAT)) {
				std::println("apple-ans2 {}: TCB invalidation failed", _ctrl->location_);
				assert(false);
			}
		}

		_head++;

		if (_head == depth_) {
			_head = 0;
			_phase ^= 1;
		}

		if (_queueType == QueueType::admin) {
			arch::scalar_store<uint32_t>(_ctrl->_nvmeMmio, NVME_DB_ACQ, _head);
		} else {
			arch::scalar_store<uint32_t>(_ctrl->_nvmeMmio, NVME_DB_IOCQ, _head);
		}

		co_return;
	}
}

} // namespace nvme
