apple_sources = files('apple.S', 'apple.cpp')
objcopy = find_program('aarch64-managarm-objcopy')

exe = executable('eir-apple', eir_sources, apple_sources,
	'../../../../generic/elf-relocate.cpp',
	include_directories : eir_includes,
	cpp_args : [eir_cpp_args, '-DEIR_PIE'],
	link_args : [eir_link_args, '-Wl,-T,' + meson.current_source_dir() + '/link.x', '-static-pie'],
	dependencies : eir_dependencies,
	link_depends : files('link.x'),
	pie : true,
	install : true
)

tmprawbin = custom_target('eir-apple-tmp.bin',
	command : [objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@'],
	input : exe,
	output : 'eir-apple-tmp.bin',
	build_by_default : true,
	install : true,
	install_dir : get_option('bindir'),
)

rawbin = custom_target('eir-apple.bin',
	command : [meson.current_source_dir() + '/patch_eir.py', '@INPUT@', '@OUTPUT@'],
	input : tmprawbin,
	output : 'eir-apple.bin',
	build_by_default : true,
	install : true,
	install_dir : get_option('bindir'),
)
