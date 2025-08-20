#include <dtb.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/main.hpp>
#include <eir-internal/memory-layout.hpp>

namespace {

uint64_t uartBase = 0;

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

namespace eir {

void debugPrintChar(char c) {
	if (uartBase == 0) {
		return;
	}

	arch::mem_space space{(void *)uartBase};

	while (!(space.load(s5l::regs::status) & s5l::status::txEmpty)) {
	}

	space.store(s5l::regs::data, c);
}

extern "C" void eirAppleMain() {
	eirRunConstructors();

	uint64_t fbBase = 0;
	uint64_t fbWidth = 0;
	uint64_t fbHeight = 0;
	uint64_t fbStride = 0;

	if (eirDtbPtr) {
		DeviceTree dt{physToVirt<void>(eirDtbPtr)};

		frg::optional<DeviceTreeNode> socNode;
		frg::optional<DeviceTreeNode> chosenNode;
		dt.rootNode().discoverSubnodes(
		    [](auto node) {
			    auto name = frg::string_view(node.name());
			    return name == "soc" || name == "chosen";
		    },
		    [&](auto node) {
			    auto name = frg::string_view(node.name());
			    if (name == "soc") {
				    socNode = node;
			    } else {
				    chosenNode = node;
			    }
		    }
		);
		assert(socNode.has_value());
		assert(chosenNode.has_value());

		socNode->discoverSubnodes(
		    [](auto node) {
			    auto compatible = node.findProperty("compatible");
			    if (!compatible.has_value()) {
				    return false;
			    }

			    for (size_t i = 0;; i++) {
				    auto prop = compatible->asString(i);
				    if (!prop.has_value()) {
					    break;
				    }

				    if (prop == "apple,s5l-uart") {
					    return true;
				    }
			    }

			    return false;
		    },
		    [&](auto uartNode) {
			    auto bootphAll = uartNode.findProperty("bootph-all");
			    if (!bootphAll.has_value()) {
				    return;
			    }

			    auto reg = uartNode.findProperty("reg");
			    if (reg.has_value()) {
				    uartBase = reg->asU64();
			    }
		    }
		);

		chosenNode->discoverSubnodes(
		    [](auto node) {
			    auto compatible = node.findProperty("compatible");
			    if (!compatible.has_value()) {
				    return false;
			    }

			    for (size_t i = 0;; i++) {
				    auto prop = compatible->asString(i);
				    if (!prop.has_value()) {
					    break;
				    }

				    if (prop.value() == "simple-framebuffer") {
					    return true;
				    }
			    }

			    return false;
		    },
		    [&](auto fbNode) {
			    auto reg = fbNode.findProperty("reg");
			    auto width = fbNode.findProperty("width");
			    auto height = fbNode.findProperty("height");
			    auto stride = fbNode.findProperty("stride");

			    fbBase = reg->asU64();
			    fbWidth = width->asU32();
			    fbHeight = height->asU32();
			    fbStride = stride->asU32();
		    }
		);
	}

	if (fbBase != 0) {
		setFbInfo((void *)fbBase, fbWidth, fbHeight, fbStride);
	}

	EirFramebuffer fb{};
	fb.fbAddress = fbBase;
	fb.fbPitch = fbStride;
	fb.fbWidth = fbWidth;
	fb.fbHeight = fbHeight;
	fb.fbBpp = 32;
	fb.fbType = 0;

	GenericInfo info{.cmdline = nullptr, .fb = fb, .debugFlags = 0, .hasFb = fbBase != 0};
	eirGenericMain(info);
}

static initgraph::Task reserveBootUartMmio{
    &globalInitEngine,
    "apple.reserve-boot-uart-mmio",
    initgraph::Entails{getMemoryRegionsKnownStage()},
    [] {
	    if (uartBase == 0) {
		    return;
	    }

	    reserveEarlyMmio(1);
    }
};

static initgraph::Task setupBootUartMmio{
    &globalInitEngine,
    "apple.setup-boot-uart-mmio",
    initgraph::Requires{getAllocationAvailableStage()},
    initgraph::Entails{getKernelLoadableStage()},
    [] {
	    if (uartBase == 0) {
		    return;
	    }

	    auto addr = allocateEarlyMmio(1);

	    mapSingle4kPage(addr, uartBase, PageFlags::write, CachingMode::mmio);
	    mapKasanShadow(addr, 0x1000);
	    unpoisonKasanShadow(addr, 0x1000);

	    extern BootUartConfig bootUartConfig;
	    bootUartConfig.address = addr;
	    bootUartConfig.type = BootUartType::s5l;
    }
};

} // namespace eir
