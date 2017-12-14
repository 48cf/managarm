#ifndef POSIX_SUBSYSTEM_DEVICE_HPP
#define POSIX_SUBSYSTEM_DEVICE_HPP

#include "vfs.hpp"

// --------------------------------------------------------
// SharedDevice
// --------------------------------------------------------

struct DeviceOperations;

struct UnixDevice {
	UnixDevice(VfsType type)
	: _type{type} { }

	VfsType type() {
		return _type;
	}

	void assignId(DeviceId id) {
		_id = id;
	}

	DeviceId getId() {
		return _id;
	}

	virtual std::string getName() = 0;

	virtual FutureMaybe<std::shared_ptr<File>> open(std::shared_ptr<FsLink> link) = 0;

	virtual FutureMaybe<std::shared_ptr<FsLink>> mount();

private:
	VfsType _type;
	DeviceId _id;
};

// --------------------------------------------------------
// UnixDeviceManager
// --------------------------------------------------------

struct UnixDeviceManager {
	void install(std::shared_ptr<UnixDevice> device);

	std::shared_ptr<UnixDevice> get(DeviceId id);

private:
	struct Compare {
		struct is_transparent { };

		bool operator() (std::shared_ptr<UnixDevice> a, DeviceId b) const {
			return a->getId() < b;
		}
		bool operator() (DeviceId a, std::shared_ptr<UnixDevice> b) const {
			return a < b->getId();
		}

		bool operator() (std::shared_ptr<UnixDevice> a, std::shared_ptr<UnixDevice> b) const {
			return a->getId() < b->getId();
		}
	};

	std::set<std::shared_ptr<UnixDevice>, Compare> _devices;
};

std::shared_ptr<FsLink> getDevtmpfs();

extern UnixDeviceManager deviceManager;

#endif // POSIX_SUBSYSTEM_DEVICE_HPP
