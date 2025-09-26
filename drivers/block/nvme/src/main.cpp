#include <format>
#include <iostream>
#include <print>

#include <core/cmdline.hpp>
#include <frg/cmdline.hpp>
#include <map>
#include <netinet/in.h>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/svrctl/server.hpp>

#include "apple.hpp"
#include "controller.hpp"
#include "fabric/tcp.hpp"
#include "subsystem.hpp"

std::map<mbus_ng::EntityId, std::unique_ptr<nvme::Subsystem>> globalSubsystems;

namespace {

async::detached runFabrics(mbus_ng::EntityId entity_id) {
	Cmdline cmdHelper;
	bool use_fabric = false;
	auto cmdline = co_await cmdHelper.get();
	frg::string_view server = "";

	frg::array args = {
	    frg::option{"nvme.over-fabric", frg::store_true(use_fabric)},
	    frg::option{"netserver.server", frg::as_string_view(server)},
	};
	frg::parse_arguments(cmdline.c_str(), args);

	if (!use_fabric)
		co_return;

	std::string remote = std::string{server.data(), server.size()};
	std::cout << std::format("block/nvme: using NVMe-over-fabric to {}\n", remote);

	auto entity = co_await mbus_ng::Instance::global().getEntity(entity_id);
	auto netserverLane = (co_await entity.getRemoteLane()).unwrap();

	auto convert_ip = [](frg::string_view &str, in_addr *addr) -> bool {
		std::string strbuf{str.data(), str.size()};
		return inet_pton(AF_INET, strbuf.c_str(), addr) == 1;
	};

	in_addr server_ip{};
	convert_ip(server, &server_ip);

	auto nvme_subsystem = std::make_unique<nvme::Subsystem>();
	co_await nvme_subsystem->run();

	auto addr = std::format("traddr={},trsvcid={},src_addr=127.0.0.1", remote, 4420);

	auto controller =
	    std::make_unique<nvme::fabric::Tcp>(-1, server_ip, 4420, addr, std::move(netserverLane));
	controller->run(nvme_subsystem->id());
	nvme_subsystem->addController(entity_id, std::move(controller));
	globalSubsystems.insert({nvme_subsystem->id(), std::move(nvme_subsystem)});
}

async::result<protocols::svrctl::Error> bindDevice(int64_t base_id) {
	for (auto &[_, subsys] : globalSubsystems) {
		if (subsys->controllers().contains(base_id))
			co_return protocols::svrctl::Error::success;
	}

	auto entity = co_await mbus_ng::Instance::global().getEntity(base_id);

	auto properties = (co_await entity.getProperties()).unwrap();
	auto classval = std::get_if<mbus_ng::StringItem>(&properties["class"]);
	auto subsystem = std::get_if<mbus_ng::StringItem>(&properties["unix.subsystem"]);

	if (classval && classval->value == "netserver") {
		runFabrics(base_id);
	} else if (subsystem && subsystem->value == "pci") {
		if (std::get<mbus_ng::StringItem>(properties.at("pci-class")).value != "01"
		    || std::get<mbus_ng::StringItem>(properties.at("pci-subclass")).value != "08"
		    || std::get<mbus_ng::StringItem>(properties.at("pci-interface")).value != "02")
			co_return protocols::svrctl::Error::deviceNotSupported;

		protocols::hw::Device device((co_await entity.getRemoteLane()).unwrap());
		co_await device.enableDma();
		auto info = co_await device.getPciInfo();

		auto &barInfo = info.barInfo[0];
		assert(barInfo.ioType == protocols::hw::IoType::kIoTypeMemory);
		auto bar0 = co_await device.accessBar(0);

		auto loc = std::format(
		    "{}:{}:{}.{}",
		    std::get<mbus_ng::StringItem>(properties.at("pci-segment")).value,
		    std::get<mbus_ng::StringItem>(properties.at("pci-bus")).value,
		    std::get<mbus_ng::StringItem>(properties.at("pci-slot")).value,
		    std::get<mbus_ng::StringItem>(properties.at("pci-function")).value
		);

		helix::Mapping mapping{bar0, barInfo.offset, barInfo.length};

		auto nvme_subsystem = std::make_unique<nvme::Subsystem>();
		co_await nvme_subsystem->run();
		auto controller = std::make_unique<PciExpressController>(
		    base_id, std::move(device), loc, std::move(mapping)
		);
		controller->run(nvme_subsystem->id());
		nvme_subsystem->addController(base_id, std::move(controller));
		globalSubsystems.insert({nvme_subsystem->id(), std::move(nvme_subsystem)});
	} else if (subsystem && subsystem->value == "dt") {
		if (properties.contains("dt.compatible=apple,nvme-ans2")) {
			protocols::hw::Device device((co_await entity.getRemoteLane()).unwrap());

			auto sartProp = co_await device.getDtProperty("apple,sart");
			if (!sartProp) {
				std::println("block/nvme: No apple,sart property found");
				co_return protocols::svrctl::Error::deviceNotSupported;
			}

			auto sartPhandle = sartProp->asU32();
			auto filter = mbus_ng::EqualsFilter{"dt.phandle", std::format("{:x}", sartPhandle)};
			auto enumerator = mbus_ng::Instance::global().enumerate(filter);
			auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
			if (events.size() != 1) {
				std::println("block/nvme: Failed to find SART device");
				co_return protocols::svrctl::Error::deviceNotSupported;
			}

			auto mboxChannel = co_await device.accessMailbox(0);
			if (!mboxChannel) {
				std::println("block/nvme: Failed to access mailbox");
				co_return protocols::svrctl::Error::deviceNotSupported;
			}

			auto sartEntity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
			auto sartDevice = protocols::hw::Device((co_await sartEntity.getRemoteLane()).unwrap());
			auto sart = co_await apple::Sart::create(std::move(sartDevice));

			struct SartWithAllocator {
				apple::Sart *sart;
				arch::contiguous_pool pool{arch::contiguous_pool_options{.addressBits = 64}};
			};

			auto sartWithAllocator = new SartWithAllocator{sart.get()};
			auto nvmeRtkitOps = new apple::RtKitOperations{
			    .arg = sartWithAllocator,
			    .shmemSetup = [](void *arg, apple::RtKitBuffer &buffer) -> bool {
				    auto sart = (SartWithAllocator *)arg;
				    auto allocateWithAlignment = [](size_t size, size_t alignment) {
					    size_t alignedSize = (size + alignment - 1) & ~(alignment - 1);

					    HelHandle memory;
					    HEL_CHECK(
					        helAllocateMemory(alignedSize, kHelAllocContinuous, nullptr, &memory)
					    );

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

				    auto address = allocateWithAlignment(buffer.size, 0x4000);
				    auto physical = helix::addressToPhysical(address);

				    if (!sart->sart->allowRegion(physical, buffer.size)) {
					    std::println("apple-rtkit: Failed to allow region in SART");
					    return false;
				    }

				    std::println(
				        "apple-rtkit: Allocated shared memory at {:#x}, size={}",
				        physical,
				        buffer.size
				    );

				    buffer.buffer = address;
				    buffer.deviceAddress = physical;

				    return true;
			    },
			};

			auto rtkit = std::make_unique<apple::RtKit>(std::move(*mboxChannel), nvmeRtkitOps);

			auto dtInfo = co_await device.getDtInfo();
			auto location = std::format("dt.{:x}", dtInfo.regs[0].address);
			auto nvmeReg = co_await device.accessDtRegister(0);
			auto ansReg = co_await device.accessDtRegister(1);
			auto nvmeMapping =
			    helix::Mapping{std::move(nvmeReg), dtInfo.regs[0].offset, dtInfo.regs[0].length};
			auto ansMapping =
			    helix::Mapping{std::move(ansReg), dtInfo.regs[1].offset, dtInfo.regs[1].length};

			co_await device.enableBusIrq();

			auto subsystem = std::make_unique<nvme::Subsystem>();
			co_await subsystem->run();

			auto controller = std::make_unique<nvme::AppleAns2NvmeController>(
			    base_id,
			    std::move(location),
			    std::move(sart),
			    std::move(rtkit),
			    std::move(nvmeMapping),
			    std::move(ansMapping)
			);
			controller->run(subsystem->id());
			subsystem->addController(base_id, std::move(controller));
			globalSubsystems.insert({subsystem->id(), std::move(subsystem)});
			co_return protocols::svrctl::Error::success;
		} else {
			co_return protocols::svrctl::Error::deviceNotSupported;
		}
	} else {
		co_return protocols::svrctl::Error::deviceNotSupported;
	}

	co_return protocols::svrctl::Error::success;
}

constexpr protocols::svrctl::ControlOperations controlOps = {
    .bind = bindDevice,
};

} // namespace

int main() {
	std::cout << "block/nvme: Starting driver\n";

	async::detach(protocols::svrctl::serveControl(&controlOps));
	async::run_forever(helix::currentDispatcher);
}
