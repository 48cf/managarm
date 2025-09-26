inc = [ 'include' ]

libapple_rtkit = shared_library('apple_rtkit', 'src/rtkit.cpp',
	dependencies : [ libarch, mbus_proto_dep, hw_proto_dep, mailboxd_dep ],
	include_directories : inc,
	install : true
)

libapple_rtkit_dep = declare_dependency(
	link_with : libapple_rtkit,
	dependencies : [ libarch, mbus_proto_dep, hw_proto_dep ],
	include_directories : inc
)

install_headers('include/apple/rtkit.hpp',
	subdir : 'drivers/libapple-rtkit/apple'
)
