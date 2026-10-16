#include <elf.h>
#include <thor-internal/debug.hpp>
#include <thor-internal/elf-loader.hpp>

namespace thor {

coroutine<ImageInfo> loadModuleImage(
	smarter::shared_ptr<Hierarchy> hierarchy,
	smarter::shared_ptr<AddressSpace, BindableHandle> space,
	VirtualAddr base,
	smarter::shared_ptr<MemoryView> image
) {
	ImageInfo info;

	// parse the ELf file format
	Elf64_Ehdr ehdr;
	auto copyEhdrOutcome = co_await image->copyFrom(0, &ehdr, sizeof(Elf64_Ehdr));
	assert(copyEhdrOutcome);
	assert(ehdr.e_ident[0] == 0x7F
			&& ehdr.e_ident[1] == 'E'
			&& ehdr.e_ident[2] == 'L'
			&& ehdr.e_ident[3] == 'F');
	// Segments are mapped where the image asks for them plus base, and nothing
	// applies a relocation, so loading at base zero only works for ET_EXEC.
	assert(base || ehdr.e_type == ET_EXEC);

	info.entryIp = reinterpret_cast<void *>(base + ehdr.e_entry);
	info.phdrEntrySize = ehdr.e_phentsize;
	info.phdrCount = ehdr.e_phnum;

	for(int i = 0; i < ehdr.e_phnum; i++) {
		Elf64_Phdr phdr;
		auto copyPhdrOutcome = co_await image->copyFrom(ehdr.e_phoff + i * ehdr.e_phentsize,
				&phdr, sizeof(Elf64_Phdr));
		assert(copyPhdrOutcome);

		if(phdr.p_type == PT_LOAD) {
			assert(phdr.p_memsz > 0);

			// A segment has to be readable, and writable-and-executable at the
			// same time is never intended.
			if(!(phdr.p_flags & PF_R) || ((phdr.p_flags & PF_W) && (phdr.p_flags & PF_X)))
				panicLogger() << "Illegal combination of segment permissions"
						<< frg::endlog;

			// align virtual address and length to page size
			uintptr_t virt_address = phdr.p_vaddr;
			virt_address -= virt_address % kPageSize;

			size_t virt_length = (phdr.p_vaddr + phdr.p_memsz) - virt_address;
			if((virt_length % kPageSize) != 0)
				virt_length += kPageSize - virt_length % kPageSize;

			// AllocatedMemory hands out zeroed pages, which is what makes the
			// part of the segment beyond p_filesz (i.e. .bss) come out zero.
			auto memoryOutcome = AllocatedMemory::create(hierarchy, virt_length);
			if(!memoryOutcome)
				panicLogger() << "thor: Failed to create memory" << frg::endlog;
			auto memory = std::move(*memoryOutcome);
			auto copyResult = co_await copyBetweenViews(memory.get(), phdr.p_vaddr - virt_address,
					image.get(), phdr.p_offset, phdr.p_filesz);
			assert(copyResult);

			auto viewOutcome = MemorySlice::create(std::move(memory), 0, virt_length);
			if(!viewOutcome)
				panicLogger() << "thor: Failed to create memory slice" << frg::endlog;

			uint32_t mapFlags = AddressSpace::kMapFixed | AddressSpace::kMapProtRead;
			if(phdr.p_flags & PF_W)
				mapFlags |= AddressSpace::kMapProtWrite;
			if(phdr.p_flags & PF_X)
				mapFlags |= AddressSpace::kMapProtExecute;

			auto mapResult = co_await space->map(std::move(*viewOutcome),
					base + virt_address, 0, virt_length, mapFlags);
			assert(mapResult);
		}else if(phdr.p_type == PT_INTERP) {
			info.interpreter.resize(phdr.p_filesz);
			auto copyInterpOutcome = co_await image->copyFrom(phdr.p_offset,
					info.interpreter.data(), phdr.p_filesz);
			assert(copyInterpOutcome);
		}else if(phdr.p_type == PT_PHDR) {
			info.phdrPtr = reinterpret_cast<void *>(base + phdr.p_vaddr);
		}else if(phdr.p_type == PT_DYNAMIC
				|| phdr.p_type == PT_TLS
				|| phdr.p_type == PT_GNU_EH_FRAME
				|| phdr.p_type == PT_GNU_STACK
				|| phdr.p_type == PT_GNU_RELRO
				|| phdr.p_type == PT_NOTE) {
			// ignore the phdr
		}else{
			warningLogger() << "Unexpected PHDR of type 0x" << frg::hex_fmt{phdr.p_type} << frg::endlog;
		}
	}

	co_return info;
}

} // namespace thor
