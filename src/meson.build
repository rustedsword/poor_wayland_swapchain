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

# The Vulkan images take the functions of the caller, so only the headers are needed
vulkan_dep = dependency('vulkan', required: false)
swapchain_sources = client_protocol_h + protocol_c + [ 'swapchain.c', 'swapchain-gbm.c' ]
swapchain_deps = [ client_dep, gbm_dep, poor_loop_dep ]
if vulkan_dep.found()
	swapchain_sources += 'swapchain-vulkan.c'
	swapchain_deps += vulkan_dep.partial_dependency(compile_args: true, includes: true)
endif

swapchain = library(
	'poor_wayland_swapchain',
	sources: swapchain_sources,
	version: meson.project_version(),
	soversion: '0',
	dependencies: swapchain_deps,
	install: true
)

swapchain_dep = declare_dependency(
	link_with: swapchain,
	include_directories: include_directories('.'),
	dependencies: swapchain_deps,
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

if vulkan_dep.found()
	pkgconfig.generate(
		name: 'poor_wayland_swapchain_vulkan',
		description: 'Vulkan images for poor_wayland_swapchain',
		version: meson.project_version(),
		filebase: 'poor_wayland_swapchain_vulkan',
		requires: [ 'poor_wayland_swapchain', 'vulkan' ]
	)
	meson.override_dependency('poor_wayland_swapchain_vulkan',
		declare_dependency(dependencies: [ swapchain_dep, vulkan_dep ]))
	install_headers('poor_wayland_swapchain_vulkan.h')
endif
