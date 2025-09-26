#pragma once

#include "controller.hpp"

#include <apple/rtkit.hpp>
#include <apple/sart.hpp>

#include <arch/barrier.hpp>
#include <async/mutex.hpp>

namespace nvme {

struct AppleAns2NvmeQueue;

struct AppleAns2NvmeController final : public Controller {
	AppleAns2NvmeController(
	    int64_t parentId,
	    std::string location,
	    std::unique_ptr<apple::Sart> sart,
	    std::unique_ptr<apple::RtKit> rtkit,
	    helix::Mapping nvmeMapping,
	    helix::Mapping ansMapping
	);

	async::detached run(mbus_ng::EntityId subsystem) override;

	async::result<Command::Result> submitAdminCommand(std::unique_ptr<Command> cmd) override;
	async::result<Command::Result> submitIoCommand(std::unique_ptr<Command> cmd) override;

private:
	async::result<void> _disable();
	async::result<void> _enable();

private:
	friend struct AppleAns2NvmeQueue;

	helix::Mapping _nvmeMapping;
	helix::Mapping _ansMapping;

	arch::dma_barrier _barrier{false};
	arch::mem_space _nvmeMmio;
	arch::mem_space _ansMmio;

	std::unique_ptr<apple::Sart> _sart;
	std::unique_ptr<apple::RtKit> _rtkit;
};

struct AppleAns2NvmeQueue final : public Queue {
	AppleAns2NvmeQueue(AppleAns2NvmeController *ctrl, QueueType queueType);

	async::result<void> init() override;
	async::detached run() override;

	async::result<Command::Result> submitCommand(std::unique_ptr<Command> cmd) override;

private:
	async::result<void> submitCommandToDevice(std::unique_ptr<Command> command);

private:
	struct NvmmuTcb {
		uint8_t opcode;
		uint8_t flags;
		uint8_t slot;
		uint8_t unk0;
		uint32_t len;
		uint64_t unk1[2];
		uint64_t prp1;
		uint64_t prp2;
		uint64_t unk2[2];
		uint8_t aesIv[8];
		uint8_t aesUnk[64];
	};

	// struct Ans2Command {
	// 	uint8_t opcode;
	// 	uint8_t flags;
	// 	uint8_t tag;
	// 	uint8_t rsvd;
	// 	uint32_t nsid;
	// 	uint32_t cdw2;
	// 	uint32_t cdw3;
	// 	uint64_t metadata;
	// 	uint64_t prp1;
	// 	uint64_t prp2;
	// 	uint32_t cdw10;
	// 	uint32_t cdw11;
	// 	uint32_t cdw12;
	// 	uint32_t cdw13;
	// 	uint32_t cdw14;
	// 	uint32_t cdw15;
	// };

	static_assert(sizeof(NvmmuTcb) == 128);
	// static_assert(sizeof(Ans2Command) == 64);

	QueueType _queueType;
	AppleAns2NvmeController *_ctrl;

	async::mutex _mutex;

	arch::dma_barrier _barrier{false};
	arch::contiguous_pool _dmaPool{arch::contiguous_pool_options{.addressBits = 64}};

	NvmmuTcb *_tcbs;
	spec::Command *_cmds;
	spec::CompletionEntry *_completions;

	size_t _head;
	size_t _phase;
};

} // namespace nvme
