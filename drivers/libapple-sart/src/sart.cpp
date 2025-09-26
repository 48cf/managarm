#include <print>

#include <arch/bit.hpp>
#include <arch/register.hpp>
#include <arch/variable.hpp>

#include <apple/sart.hpp>

namespace {

constexpr size_t maxSartEntries = 16;

namespace sartv2 {

constexpr size_t sizeShift = 12;
constexpr size_t addressShift = 12;

constexpr auto configReg(size_t index) { return arch::bit_register<uint32_t>(0x00 + index * 4); }
constexpr auto addressReg(size_t index) {
	return arch::scalar_register<uint32_t>(0x40 + index * 4);
}

namespace config {

constexpr arch::field<uint32_t, uint32_t> size{0, 24};
constexpr arch::field<uint32_t, uint8_t> flags{24, 8};

} // namespace config

} // namespace sartv2

struct SartV2 final : public apple::Sart {
	SartV2(std::string location, helix::Mapping mapping)
	: Sart{std::move(location), std::move(mapping)} {
		_setupProtectedMappings();
	}

	std::optional<Entry> _getEntry(size_t index) override {
		auto config = _mmio.load(sartv2::configReg(index));
		auto addr = _mmio.load(sartv2::addressReg(index));

		return Entry{
		    .flags = config & sartv2::config::flags,
		    .address = addr << sartv2::addressShift,
		    .length = (config & sartv2::config::size) << sartv2::sizeShift,
		};
	}

	bool _setEntry(size_t index, Entry entry) override {
		_mmio.store(sartv2::addressReg(index), entry.address >> sartv2::addressShift);
		_mmio.store(
		    sartv2::configReg(index),
		    sartv2::config::size(entry.length >> sartv2::sizeShift)
		        | sartv2::config::flags(entry.flags)
		);

		return true;
	}
};

} // namespace

namespace apple {

Sart::Sart(std::string location, helix::Mapping mapping)
: _location{std::move(location)},
  _mapping{std::move(mapping)},
  _mmio{_mapping.get()} {}

void Sart::_setupProtectedMappings() {
	_protectedMappings = 0;

	for (size_t i = 0; i < maxSartEntries; i++) {
		auto entry = _getEntry(i);
		if (!entry->flags) {
			continue;
		}

		std::println(
		    "apple-sart {}: Entry {} reserved (flags={:#x}, addr={:#x}, len={:#x})",
		    _location,
		    i,
		    entry->flags,
		    entry->address,
		    entry->length
		);

		_protectedMappings |= 1 << i;
	}
}

bool Sart::allowRegion(uint64_t address, size_t length) {
	for (size_t i = 0; i < maxSartEntries; i++) {
		if ((_protectedMappings & (1 << i)) != 0) {
			continue;
		}

		auto entry = _getEntry(i);
		if (entry->flags != 0) {
			continue;
		}

		entry->flags = 0xFF; // Probably a bitfield but the exact meaning of each bit is unknown.
		entry->address = address;
		entry->length = length;

		return _setEntry(i, *entry);
	}

	std::println("apple-sart {}: No free SART entries available", _location);
	return false;
}

bool Sart::disallowRegion(uint64_t address, size_t length) {
	for (size_t i = 0; i < maxSartEntries; i++) {
		if ((_protectedMappings & (1 << i)) == 0) {
			continue;
		}

		auto entry = _getEntry(i);
		if (!entry->flags || entry->address != address || entry->length != length) {
			continue;
		}

		entry->flags = 0;
		entry->address = 0;
		entry->length = 0;

		return _setEntry(i, *entry);
	}

	std::println("apple-sart {}: No matching SART entry found", _location);
	return false;
}

async::result<std::unique_ptr<Sart>> Sart::create(protocols::hw::Device device) {
	auto dtInfo = co_await device.getDtInfo();
	if (dtInfo.regs.size() != 1) {
		std::println("apple-sart: Unexpected number of registers");
		co_return {};
	}

	auto location = std::format("dt.{:x}", dtInfo.regs[0].address);

	auto properties = co_await device.getDtProperties();
	auto compatible = std::find_if(properties.begin(), properties.end(), [](const auto &prop) {
		return prop.first == "compatible";
	});
	if (compatible == properties.end()) {
		std::println("apple-sart {}: No compatible property found", location);
		co_return {};
	}

	uint8_t sartVersion = 0;
	for (size_t i = 0;; i++) {
		auto compatibleString = compatible->second.asString(i);
		if (!compatibleString) {
			break;
		}

		if (compatibleString == "apple,t8103-sart") {
			sartVersion = 2;
			break;
		}
	}

	if (sartVersion == 0) {
		std::println("apple-sart {}: No supported compatible string found", location);
		co_return {};
	} else if (sartVersion != 2) {
		std::println("apple-sart {}: Unsupported SART version {}", location, sartVersion);
		co_return {};
	}

	auto reg = co_await device.accessDtRegister(0);
	auto mapping = helix::Mapping{std::move(reg), dtInfo.regs[0].offset, dtInfo.regs[0].length};

	std::println("apple-sart {}: Found SART version {}", location, sartVersion);
	std::println(
	    "apple-sart {}: MMIO register at address 0x{:x}, length 0x{:x}",
	    location,
	    dtInfo.regs[0].address,
	    dtInfo.regs[0].length
	);

	if (sartVersion == 2) {
		co_return std::make_unique<SartV2>(std::move(location), std::move(mapping));
	}

	co_return nullptr;
}

} // namespace apple
