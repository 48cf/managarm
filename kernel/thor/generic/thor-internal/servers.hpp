#pragma once

#include <thor-internal/coroutine.hpp>
#include <thor-internal/universe.hpp>
#include "svrctl.frigg_bragi.hpp"

namespace thor {

void initializeSvrctl();
void initializeMbusStream();
coroutine<void> initPosixEmulation();

// The two ends of the mbus lane. The server end can only be taken once; it
// goes to freya, which launches mbus.
smarter::shared_ptr<Stream, LanePolicy> takeMbusServerLane();
smarter::shared_ptr<Stream, LanePolicy> getMbusClientLane();

coroutine<smarter::shared_ptr<Stream, LanePolicy>> runServer(
	managarm::svrctl::Description<KernelAlloc> &desc
);

// Parse a server description that is stored on the initrd.
coroutine<managarm::svrctl::Description<KernelAlloc>> parseServerFromInitrd(frg::string_view path);

// Launch from a server description stored on the initrd.
coroutine<smarter::shared_ptr<Stream, LanePolicy>> runServerFromInitrd(frg::string_view path);

} // namespace thor
