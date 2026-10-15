swapchain_xmls = {
	'linux_dmabuf_v1': protocols_dir / 'stable/linux-dmabuf/linux-dmabuf-v1.xml',
	'linux_drm_syncobj_v1': protocols_dir / 'staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml',
}

client_protocol_h = []
server_protocol_h = []
protocol_c = []
foreach name, xml : swapchain_xmls
	client_protocol_h += custom_target(
		name + ' client header',
		command: [ scanner, '-s', 'client-header', '@INPUT@', '@OUTPUT@' ],
		input: xml,
		output: 'poor_' + name + '_client_protocol.h',
		install: true,
		install_dir: get_option('includedir'),
	)
	server_protocol_h += custom_target(
		name + ' server header',
		command: [ scanner, '-s', 'server-header', '@INPUT@', '@OUTPUT@' ],
		input: xml,
		output: 'poor_' + name + '_server_protocol.h',
	)
	protocol_c += custom_target(
		name + ' protocol source',
		command: [ scanner, '-s', 'public-code', '@INPUT@', '@OUTPUT@' ],
		input: xml,
		output: 'poor_' + name + '_protocol.c',
	)
endforeach

swapchain = library(
	'poor_wayland_swapchain',
	sources: client_protocol_h + protocol_c + [ 'swapchain.c', 'swapchain-gbm.c' ],
	version: meson.project_version(),
	soversion: '0',
	dependencies: [ client_dep, gbm_dep, poor_loop_dep ],
	install: true
)

swapchain_dep = declare_dependency(
	link_with: swapchain,
	include_directories: include_directories('.'),
	dependencies: [ client_dep, gbm_dep, poor_loop_dep ],
	sources: client_protocol_h
)

pkgconfig.generate(
	swapchain,
	name: 'poor_wayland_swapchain',
	description: 'dmabuf swapchain for poor_wayland clients',
	version: meson.project_version(),
	filebase: 'poor_wayland_swapchain',
	requires: [ 'poor_wayland_client', 'gbm' ]
)

meson.override_dependency('poor_wayland_swapchain', swapchain_dep)

install_headers('poor_wayland_swapchain.h')
