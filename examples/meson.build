xdg_shell_xml = protocols_dir / 'stable/xdg-shell/xdg-shell.xml'

xdg_shell_client_protocol_h = custom_target(
	'xdg-shell client header',
	command: [ scanner, '-s', 'client-header', '@INPUT@', '@OUTPUT@' ],
	input: xdg_shell_xml,
	output: 'xdg-shell-client-protocol.h',
)

xdg_shell_protocol_c = custom_target(
	'xdg-shell protocol source',
	command: [ scanner, '-s', 'private-code', '@INPUT@', '@OUTPUT@' ],
	input: xdg_shell_xml,
	output: 'xdg-shell-protocol.c',
)

executable(
	'simple-shm',
	'simple-shm.c',
	'example-log.c',
	xdg_shell_client_protocol_h,
	xdg_shell_protocol_c,
	dependencies: [ client_dep, poor_loop_dep ],
)

egl_dep = dependency('egl', required: false)
glesv2_dep = dependency('glesv2', required: false)

if egl_dep.found() and glesv2_dep.found()
	executable(
		'simple-egl',
		'simple-egl.c',
		'example-log.c',
		xdg_shell_client_protocol_h,
		xdg_shell_protocol_c,
		dependencies: [ swapchain_dep, egl_dep, glesv2_dep ],
	)
endif
