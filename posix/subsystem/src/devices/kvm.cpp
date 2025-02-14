#include <string.h>
#include <linux/kvm.h>

#include "../common.hpp"
#include "../process.hpp"
#include "null.hpp"

#include <bragi/helpers-std.hpp>

#include <bitset>
#include <coroutine>

namespace {

struct KvmVmFile;
struct KvmCpuFile;
struct KvmFile;

struct KvmRunState {
	struct kvm_run run;
	// Scratch space used for IO
	char scratch[64];
};

struct KvmCpuFile final : File {
private:
	static HelX86SegmentRegister convertSegmentRegister(managarm::fs::KvmSegment &segment) {
		HelX86SegmentRegister reg;
		reg.base = segment.base();
		reg.limit = segment.limit();
		reg.selector = segment.selector();
		reg.type = segment.type();
		reg.present = segment.present();
		reg.dpl = segment.dpl();
		reg.db = segment.db();
		reg.s = segment.s();
		reg.l = segment.l();
		reg.g = segment.g();
		reg.avl = segment.avl();
		return reg;
	}

	static HelX86DescriptorTable convertDescriptorTable(managarm::fs::KvmDtable &dtable) {
		HelX86DescriptorTable tab;
		tab.base = dtable.base();
		tab.limit = dtable.limit();
		return tab;
	}

	static managarm::fs::KvmSegment convertSegment(const HelX86SegmentRegister &reg) {
		managarm::fs::KvmSegment segment;
		segment.set_base(reg.base);
		segment.set_limit(reg.limit);
		segment.set_selector(reg.selector);
		segment.set_type(reg.type);
		segment.set_present(reg.present);
		segment.set_dpl(reg.dpl);
		segment.set_db(reg.db);
		segment.set_s(reg.s);
		segment.set_l(reg.l);
		segment.set_g(reg.g);
		segment.set_avl(reg.avl);
		return segment;
	}

	managarm::fs::KvmDtable convertDescriptorTable(const HelX86DescriptorTable &tab) {
		managarm::fs::KvmDtable dtable;
		dtable.set_base(tab.base);
		dtable.set_limit(tab.limit);
		return dtable;
	}

	FutureMaybe<helix::UniqueDescriptor> accessMemory() override {
		co_return vcpuMemory_.getHandle();
	}

	async::result<void> ioctl(Process *, uint32_t id, helix_ng::RecvInlineResult msg,
			helix::UniqueLane conversation) override {
		switch(id) {
			case managarm::fs::KvmVcpuGetSpecialRegistersRequest::message_id: {
				auto req = bragi::parse_head_only<managarm::fs::KvmVcpuGetSpecialRegistersRequest>(msg);
				assert(req);

				HelX86VirtualizationRegs regs;
				auto result = helLoadRegisters(vcpuHandle_, kHelRegsVirtualization, &regs);
				HEL_CHECK(result);

				managarm::fs::KvmVcpuGetSpecialRegistersReply reply;
				reply.set_error(managarm::fs::Errors::SUCCESS);

				auto &reply_regs = reply.regs();

				reply_regs.set_cs(convertSegment(regs.cs));
				reply_regs.set_ds(convertSegment(regs.ds));
				reply_regs.set_es(convertSegment(regs.es));
				reply_regs.set_fs(convertSegment(regs.fs));
				reply_regs.set_gs(convertSegment(regs.gs));
				reply_regs.set_ss(convertSegment(regs.ss));
				reply_regs.set_tr(convertSegment(regs.tr));
				reply_regs.set_ldt(convertSegment(regs.ldt));

				reply_regs.set_gdt(convertDescriptorTable(regs.gdt));
				reply_regs.set_idt(convertDescriptorTable(regs.idt));

				reply_regs.set_cr0(regs.cr0);
				reply_regs.set_cr2(regs.cr2);
				reply_regs.set_cr3(regs.cr3);
				reply_regs.set_cr4(regs.cr4);
				reply_regs.set_cr8(regs.cr8);
				reply_regs.set_efer(regs.efer);
				reply_regs.set_apic_base(regs.apic_base);

				auto [sendResp, sendTail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadTail(reply, frg::stl_allocator{})
				);
				HEL_CHECK(sendResp.error());
				HEL_CHECK(sendTail.error());

				co_return;
			}
			case managarm::fs::KvmVcpuSetSpecialRegistersRequest::message_id: {
				auto preamble = bragi::read_preamble(msg);

				std::vector<uint8_t> tail(preamble.tail_size());

				auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
				HEL_CHECK(recv_tail.error());

				auto req = bragi::parse_head_tail<managarm::fs::KvmVcpuSetSpecialRegistersRequest>(msg, tail);
				assert(req);

				HelX86VirtualizationRegs regs;
				auto result = helLoadRegisters(vcpuHandle_, kHelRegsVirtualization, &regs);
				HEL_CHECK(result);

				auto &req_regs = req->regs();

				regs.cr0 = req_regs.cr0();
				regs.cr2 = req_regs.cr2();
				regs.cr3 = req_regs.cr3();
				regs.cr4 = req_regs.cr4();
				regs.cr8 = req_regs.cr8();
				regs.efer = req_regs.efer();
				regs.apic_base = req_regs.apic_base();

				regs.cs = convertSegmentRegister(req_regs.cs());
				regs.ds = convertSegmentRegister(req_regs.ds());
				regs.es = convertSegmentRegister(req_regs.es());
				regs.fs = convertSegmentRegister(req_regs.fs());
				regs.gs = convertSegmentRegister(req_regs.gs());
				regs.ss = convertSegmentRegister(req_regs.ss());
				regs.tr = convertSegmentRegister(req_regs.tr());
				regs.ldt = convertSegmentRegister(req_regs.ldt());

				regs.gdt = convertDescriptorTable(req_regs.gdt());
				regs.idt = convertDescriptorTable(req_regs.idt());

				result = helStoreRegisters(vcpuHandle_, kHelRegsVirtualization, &regs);
				HEL_CHECK(result);

				managarm::fs::KvmVcpuSetSpecialRegistersReply reply;
				reply.set_error(managarm::fs::Errors::SUCCESS);

				auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(reply, frg::stl_allocator{})
				);
				HEL_CHECK(sendResp.error());
				co_return;
			}
			case managarm::fs::KvmVcpuGetRegistersRequest::message_id: {
				auto req = bragi::parse_head_only<managarm::fs::KvmVcpuGetRegistersRequest>(msg);
				assert(req);

				HelX86VirtualizationRegs regs;
				auto result = helLoadRegisters(vcpuHandle_, kHelRegsVirtualization, &regs);
				HEL_CHECK(result);

				managarm::fs::KvmVcpuGetRegistersReply reply;
				reply.set_error(managarm::fs::Errors::SUCCESS);

				auto &reply_regs = reply.regs();

				reply_regs.set_rax(regs.rax);
				reply_regs.set_rbx(regs.rbx);
				reply_regs.set_rcx(regs.rcx);
				reply_regs.set_rdx(regs.rdx);
				reply_regs.set_rsi(regs.rsi);
				reply_regs.set_rdi(regs.rdi);
				reply_regs.set_rbp(regs.rbp);
				reply_regs.set_r8(regs.r8);
				reply_regs.set_r9(regs.r9);
				reply_regs.set_r10(regs.r10);
				reply_regs.set_r11(regs.r11);
				reply_regs.set_r12(regs.r12);
				reply_regs.set_r13(regs.r13);
				reply_regs.set_r14(regs.r14);
				reply_regs.set_r15(regs.r15);
				reply_regs.set_rsp(regs.rsp);
				reply_regs.set_rip(regs.rip);
				reply_regs.set_rflags(regs.rflags);

				auto [sendResp, sendTail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadTail(reply, frg::stl_allocator{})
				);
				HEL_CHECK(sendResp.error());
				HEL_CHECK(sendTail.error());
				co_return;
			}
			case managarm::fs::KvmVcpuSetRegistersRequest::message_id: {
				auto preamble = bragi::read_preamble(msg);

				std::vector<uint8_t> tail(preamble.tail_size());

				auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
				HEL_CHECK(recv_tail.error());

				auto req = bragi::parse_head_tail<managarm::fs::KvmVcpuSetRegistersRequest>(msg, tail);
				assert(req);

				HelX86VirtualizationRegs regs;
				auto result = helLoadRegisters(vcpuHandle_, kHelRegsVirtualization, &regs);
				HEL_CHECK(result);

				auto &req_regs = req->regs();

				regs.rax = req_regs.rax();
				regs.rbx = req_regs.rbx();
				regs.rcx = req_regs.rcx();
				regs.rdx = req_regs.rdx();
				regs.rsi = req_regs.rsi();
				regs.rdi = req_regs.rdi();
				regs.rbp = req_regs.rbp();
				regs.r8 = req_regs.r8();
				regs.r9 = req_regs.r9();
				regs.r10 = req_regs.r10();
				regs.r11 = req_regs.r11();
				regs.r12 = req_regs.r12();
				regs.r13 = req_regs.r13();
				regs.r14 = req_regs.r14();
				regs.r15 = req_regs.r15();
				regs.rsp = req_regs.rsp();
				regs.rip = req_regs.rip();
				regs.rflags = req_regs.rflags();

				result = helStoreRegisters(vcpuHandle_, kHelRegsVirtualization, &regs);
				HEL_CHECK(result);

				managarm::fs::KvmVcpuSetRegistersReply reply;
				reply.set_error(managarm::fs::Errors::SUCCESS);

				auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(reply, frg::stl_allocator{})
				);
				HEL_CHECK(sendResp.error());
				co_return;
			}
			case managarm::fs::KvmVcpuRunRequest::message_id: {
				HelVmexitReason reason;
				auto result = helRunVirtualizedCpu(vcpuHandle_, &reason);
				HEL_CHECK(result);

				HelX86VirtualizationRegs regs;
				result = helLoadRegisters(vcpuHandle_, kHelRegsVirtualization, &regs);
				HEL_CHECK(result);

				auto state = reinterpret_cast<KvmRunState *>(vcpuMapping_.get());
				// state->run.fail_entry.cpu

				switch(reason.exitReason) {
					case kHelVmexitHlt:
						state->run.exit_reason = KVM_EXIT_HLT;
						break;
					case kHelVmexitIo: {
						state->run.exit_reason = KVM_EXIT_IO;
						state->run.io.port = reason.address;

						if(reason.flags & kHelIoFlagString) {
							std::cout << "\e[31mposix: Unhandled KVM_EXIT_IO with string flag\e[39m" << std::endl;
							break;
						}

						if(reason.flags & kHelIoStringRep) {
							std::cout << "\e[31mposix: Unhandled KVM_EXIT_IO with rep flag\e[39m" << std::endl;
							break;
						}

						if(reason.flags & kHelIoRead) {
							state->run.io.direction = KVM_EXIT_IO_IN;
							std::cout << "\e[31mposix: Unhandled KVM_EXIT_IO with direction=IN\e[39m" << std::endl;
						} else {
							uint8_t read_size;
							state->run.io.direction = KVM_EXIT_IO_OUT;

							if(reason.flags & kHelIoWidth8) {
								read_size = 1;
								state->scratch[0] = regs.rax & 0xff;
							} else if(reason.flags & kHelIoWidth16) {
								read_size = 2;
								state->scratch[0] = regs.rax & 0xff;
								state->scratch[1] = (regs.rax >> 8) & 0xff;
							} else if(reason.flags & kHelIoWidth32) {
								read_size = 4;
								state->scratch[0] = regs.rax & 0xff;
								state->scratch[1] = (regs.rax >> 8) & 0xff;
								state->scratch[2] = (regs.rax >> 16) & 0xff;
								state->scratch[3] = (regs.rax >> 24) & 0xff;
							} else {
								std::cout << "\e[31mposix: Unhandled KVM_EXIT_IO size\e[39m" << std::endl;
								break;
							}

							state->run.io.count = 1;
							state->run.io.size = read_size;
							state->run.io.data_offset = offsetof(KvmRunState, scratch);
						}

						break;
					}
					case kHelVmexitTranslationFault:
						state->run.exit_reason = KVM_EXIT_MEMORY_FAULT;
						break;
					case static_cast<uint32_t>(kHelVmexitError):
						state->run.exit_reason = KVM_EXIT_INTERNAL_ERROR;
						break;
					case static_cast<uint32_t>(kHelVmexitUnknownPlatformSpecificExitCode):
						state->run.exit_reason = KVM_EXIT_UNKNOWN;
						break;
				}

				managarm::fs::KvmVcpuRunReply reply;
				reply.set_error(managarm::fs::Errors::SUCCESS);

				auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(reply, frg::stl_allocator{})
				);
				HEL_CHECK(sendResp.error());
				co_return;
			}
		}

		std::cout << "\e[31mposix: KvmCpuFile does not implement ioctl()\e[39m " << "request_id=" << id << std::endl;
		co_await helix_ng::exchangeMsgs(conversation, helix_ng::dismiss());
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return passthrough_;
	}

	helix::UniqueLane passthrough_;
	async::cancellation_event cancelServe_;

	HelHandle vcpuHandle_;
	smarter::shared_ptr<KvmVmFile> vm_;
	helix::UniqueDescriptor vcpuMemory_;
	helix::Mapping vcpuMapping_;

public:
	static void serve(smarter::shared_ptr<KvmCpuFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->passthrough_) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->cancelServe_));
	}

	KvmCpuFile(HelHandle vcpuHandle, smarter::shared_ptr<KvmVmFile> vm, helix::UniqueDescriptor vcpuMemory)
	: File{StructName::get("kvm-cpu-file")}, vcpuHandle_(vcpuHandle), vm_(std::move(vm)),
			vcpuMemory_(std::move(vcpuMemory)), vcpuMapping_{vcpuMemory_, 0, 0x1000} { }

	~KvmCpuFile() {
		auto result = helCloseDescriptor(kHelThisUniverse, vcpuHandle_);
		HEL_CHECK(result);
	}
};

struct KvmVmFile final : File {
private:
	async::result<void> ioctl(Process *, uint32_t id, helix_ng::RecvInlineResult msg,
			helix::UniqueLane conversation) override {
		switch(id) {
			case managarm::fs::KvmCreateVcpuRequest::message_id: {
				auto req = bragi::parse_head_only<managarm::fs::KvmCreateVcpuRequest>(msg);
				assert(req);

				auto [extract_creds] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::extractCredentials()
				);
				HEL_CHECK(extract_creds.error());

				auto process = findProcessWithCredentials(extract_creds.credentials());
				assert(process);

				managarm::fs::KvmCreateVcpuReply resp;

				HelHandle vcpuHandle;
				auto result = helCreateVirtualizedCpu(vmSpaceHandle_, &vcpuHandle);
				if(result != kHelErrNone) {
					std::cout << "\e[31mposix: Failed to create vCPU\e[39m" << std::endl;
					resp.set_error(managarm::fs::Errors::INTERNAL_ERROR);
				} else {
					size_t vcpuSize = sizeof(KvmRunState);
					vcpuSize = (vcpuSize + 0xfff) & ~size_t(0xfff);

					HelHandle vcpuMemory;
					auto result = helAllocateMemory(vcpuSize, 0, nullptr, &vcpuMemory);
					HEL_CHECK(result);

					auto file = smarter::make_shared<KvmCpuFile>(vcpuHandle,
						smarter::static_pointer_cast<KvmVmFile>(weakFile().lock()),
						helix::UniqueDescriptor{vcpuMemory});
					file->setupWeakFile(file);
					KvmCpuFile::serve(file);
					auto handle = File::constructHandle(std::move(file));
					auto vcpu_fd = process->fileContext()->attachFile(handle);
					resp.set_error(managarm::fs::Errors::SUCCESS);
					resp.set_vcpu_fd(vcpu_fd);
				}

				auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(sendResp.error());
				co_return;
			}
			case managarm::fs::KvmSetMemoryRegionRequest::message_id: {
				auto req = bragi::parse_head_only<managarm::fs::KvmSetMemoryRegionRequest>(msg);
				assert(req);

				auto [extract_creds] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::extractCredentials()
				);
				HEL_CHECK(extract_creds.error());

				auto process = findProcessWithCredentials(extract_creds.credentials());
				assert(process);

				std::cout << std::format("guest_phys_addr={:#x}, user_addr={:#x}, memory_size={:#x}, flags={:#x}",
					req->guest_phys_addr(), req->user_addr(), req->memory_size(), req->flags()) << std::endl;

				helix::BorrowedDescriptor memory_handle;
				for(auto area : *process->vmContext()) {
					std::cout << std::format("base={:#x}, length={:#x}", area.baseAddress(), area.size()) << std::endl;
					if(area.baseAddress() != req->user_addr() && area.size() != req->memory_size())
						continue;

					if(area.isPrivate()) {
						memory_handle = area.copyView();
					} else {
						memory_handle = area.fileView();
					}

					break;
				}

				managarm::fs::KvmSetMemoryRegionReply resp;
				if(memory_handle.getHandle() == kHelNullHandle) {
					resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
					std::cout << "\e[31mposix: Could not find memory region\e[39m" << std::endl;
				} else {
					uint32_t map_flags = kHelMapFixed | kHelMapProtRead | kHelMapProtExecute;
					if(!(req->flags() & KVM_MEM_READONLY))
						map_flags |= kHelMapProtWrite;

					void *fake_ptr;
					auto result = helMapMemory(memory_handle.getHandle(), vmSpaceHandle_,
						reinterpret_cast<void *>(req->guest_phys_addr()),
						0, req->memory_size(), map_flags, &fake_ptr);

					if(result != kHelErrNone) {
						resp.set_error(managarm::fs::Errors::INTERNAL_ERROR);
						std::cout << "\e[31mposix: Failed to map memory region\e[39m" << std::endl;
					} else {
						resp.set_error(managarm::fs::Errors::SUCCESS);
					}
				}

				auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(sendResp.error());
				co_return;

				// ...
			}
		}

		std::cout << "\e[31mposix: KvmVmFile does not implement ioctl()\e[39m " << "request_id=" << id << std::endl;
		co_await helix_ng::exchangeMsgs(conversation, helix_ng::dismiss());
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return passthrough_;
	}

	helix::UniqueLane passthrough_;
	async::cancellation_event cancelServe_;

	HelHandle vmSpaceHandle_;

public:
	static void serve(smarter::shared_ptr<KvmVmFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->passthrough_) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->cancelServe_));
	}

	KvmVmFile(HelHandle vmSpaceHandle)
	: File{StructName::get("kvm-vm-file")}, vmSpaceHandle_(vmSpaceHandle) { }

	~KvmVmFile() {
		auto result = helCloseDescriptor(kHelThisUniverse, vmSpaceHandle_);
		HEL_CHECK(result);
	}
};

struct KvmFile final : File {
private:
	async::result<void> ioctl(Process *, uint32_t id, helix_ng::RecvInlineResult msg,
			helix::UniqueLane conversation) override {
		switch(id) {
			case managarm::fs::KvmGetApiVersionRequest::message_id: {
				auto req = bragi::parse_head_only<managarm::fs::KvmGetApiVersionRequest>(msg);
				assert(req);

				managarm::fs::KvmGetApiVersionReply resp;
				resp.set_api_version(KVM_API_VERSION);

				auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(sendResp.error());
				co_return;
			}
			case managarm::fs::KvmGetVcpuMmapSizeRequest::message_id: {
				managarm::fs::KvmGetVcpuMmapSizeReply resp;
				resp.set_mmap_size(sizeof(KvmRunState));

				auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(sendResp.error());
				co_return;
			}
			case managarm::fs::KvmCreateVmRequest::message_id: {
				auto req = bragi::parse_head_only<managarm::fs::KvmCreateVmRequest>(msg);
				assert(req);

				auto [extract_creds] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::extractCredentials()
				);
				HEL_CHECK(extract_creds.error());

				auto process = findProcessWithCredentials(extract_creds.credentials());
				assert(process);

				managarm::fs::KvmCreateVmReply resp;

				if(req->machine_type() != 0) {
					std::cout << "\e[31mposix: /dev/kvm does not support machine types\e[39m" << std::endl;
					resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
				} else {
					HelHandle vm_space;
					if(helCreateVirtualizedSpace(&vm_space) != kHelErrNone) {
						resp.set_error(managarm::fs::Errors::INTERNAL_ERROR);
					} else {
						auto file = smarter::make_shared<KvmVmFile>(vm_space);
						file->setupWeakFile(file);
						KvmVmFile::serve(file);
						auto handle = File::constructHandle(std::move(file));
						auto vm_fd = process->fileContext()->attachFile(handle);
						resp.set_error(managarm::fs::Errors::SUCCESS);
						resp.set_vm_fd(vm_fd);
					}
				}

				auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(sendResp.error());
				co_return;
			}
		}

		std::cout << "\e[31mposix: KvmFile does not implement ioctl()\e[39m " << "request_id=" << id << std::endl;
		co_await helix_ng::exchangeMsgs(conversation, helix_ng::dismiss());
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return passthrough_;
	}

	helix::UniqueLane passthrough_;
	async::cancellation_event cancelServe_;

public:
	static void serve(smarter::shared_ptr<KvmFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->passthrough_) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->cancelServe_));
	}

	KvmFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
	: File{StructName::get("kvm-file"), std::move(mount), std::move(link)} { }
};

struct KvmDevice final : UnixDevice {
	KvmDevice()
	: UnixDevice(VfsType::charDevice) {
		assignId({10, 232});
	}

	std::string nodePath() override {
		return "kvm";
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		if(semantic_flags & ~(semanticRead | semanticWrite)){
			std::cout << "\e[31mposix: /dev/kvm open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
				<< std::endl;
			co_return Error::illegalArguments;
		}

		auto file = smarter::make_shared<KvmFile>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		KvmFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}
};

} // anonymous namespace

std::shared_ptr<UnixDevice> createKvmDevice() {
	return std::make_shared<KvmDevice>();
}
