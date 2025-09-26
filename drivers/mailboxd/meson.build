src = [
	'src/apple.cpp',
	'src/main.cpp',
]

mailboxd_dep = declare_dependency(
	dependencies : [ libarch, hw_proto_dep ],
	include_directories : [ 'include' ]
)

executable('mailboxd', src,
	dependencies : [ mailboxd_dep, libarch, hw_proto_dep, mbus_proto_dep, svrctl_proto_dep ],
	install : true
)

custom_target('mailboxd-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'mailboxd.bin',
	input : 'mailboxd.yml',
	install : true,
	install_dir : server
)

install_headers(
	'include/mailboxd/apple.hpp',
	'include/mailboxd/mailbox.hpp',
	subdir : 'drivers/mailboxd/mailboxd'
)
