inc = [ 'include' ]
deps = [ libarch, mbus_proto_dep, hw_proto_dep ]

libapple_sart = shared_library('apple_sart', 'src/sart.cpp',
	dependencies : deps,
	include_directories : inc,
	install : true
)

libapple_sart_dep = declare_dependency(
	link_with : libapple_sart,
	dependencies : deps,
	include_directories : inc
)

install_headers('include/apple/sart.hpp',
	subdir : 'drivers/libapple-sart/apple'
)
