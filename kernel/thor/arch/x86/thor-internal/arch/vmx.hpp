#pragma once

#include <hel.h>
#include <thor-internal/arch/ept.hpp>
#include <thor-internal/virtualization.hpp>

namespace thor::vmx {
	bool initialize();

	struct Vmcs final : VirtualizedCpu {
		Vmcs(smarter::shared_ptr<EptSpace> ept);
		~Vmcs();

		Vmcs(const Vmcs& vmcs) = delete;
		Vmcs& operator=(const Vmcs& vmcs) = delete;

		void updateHostRsp(uintptr_t rsp);

		HelVmexitReason run();
		void storeRegs(const HelX86VirtualizationRegs *regs);
		void loadRegs(HelX86VirtualizationRegs *res);

	private:
		uintptr_t _savedHostRsp = 0;

		PhysicalAddr _vmcs;
		uint8_t* _hostFstate;
		uint8_t* _guestFstate;
		GuestState _state;

		smarter::shared_ptr<EptSpace> _space;
	};
}
