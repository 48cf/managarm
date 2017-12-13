
#ifndef POSIX_SUBSYSTEM_EXTERN_FS_HPP
#define POSIX_SUBSYSTEM_EXTERN_FS_HPP

#include "vfs.hpp"

namespace extern_fs {

std::shared_ptr<Link> createRoot(helix::UniqueLane lane);
std::shared_ptr<ProperFile> createFile(helix::UniqueLane lane, std::shared_ptr<Link> link);

} // namespace extern_fs

#endif // POSIX_SUBSYSTEM_EXTERN_FS_HPP

