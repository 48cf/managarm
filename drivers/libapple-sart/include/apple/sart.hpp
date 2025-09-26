#pragma once

#include <helix/memory.hpp>
#include <protocols/hw/client.hpp>

namespace apple {

struct Sart {
	virtual ~Sart() = default;

	bool allowRegion(uint64_t address, size_t length);
	bool disallowRegion(uint64_t address, size_t length);

	static async::result<std::unique_ptr<Sart>> create(protocols::hw::Device device);

protected:
	struct Entry {
		uint8_t flags;
		uint64_t address;
		uint64_t length;
	};

	Sart(std::string location, helix::Mapping mapping);

	void _setupProtectedMappings();

	virtual std::optional<Entry> _getEntry(size_t index) = 0;
	virtual bool _setEntry(size_t index, Entry entry) = 0;

	std::string _location;
	helix::Mapping _mapping;
	arch::mem_space _mmio;
	size_t _protectedMappings;
};

} // namespace apple
