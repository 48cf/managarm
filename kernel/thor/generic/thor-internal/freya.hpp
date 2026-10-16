#pragma once

#include <thor-internal/coroutine.hpp>
#include <thor-internal/memory-view.hpp>
#include <thor-internal/stream.hpp>

namespace thor {

// Loads freya, the server manager, from the initrd and starts it.
//
// Unlike the servers that freya itself will go on to launch, freya is started
// directly: it has no server description, no POSIX lane and no observer in thor.
// Everything it needs is named by the HelBootstrapData struct that this function
// writes into its address space.
coroutine<void> runFreya(smarter::shared_ptr<MemoryView> initrdView, size_t initrdOffset,
		size_t initrdSize, smarter::shared_ptr<Stream, LanePolicy> mbusServerLane,
		smarter::shared_ptr<Stream, LanePolicy> mbusClientLane);

} // namespace thor
