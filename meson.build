project(
	'poor_wayland_swapchain', 'c',
	version: '0.1.0',
	license: 'MIT',
	meson_version: '>= 1.4.0',
	default_options: [
		'warning_level=2',
		'buildtype=debugoptimized',
		'c_std=c23',
	]
)

if host_machine.system() != 'linux'
	error('poor_wayland runs on io_uring, which needs Linux')
endif

pkgconfig = import('pkgconfig')

config_h = configuration_data()
config_h.set_quoted('PACKAGE', meson.project_name())
config_h.set_quoted('PACKAGE_VERSION', meson.project_version())

cc = meson.get_compiler('c')
add_project_arguments(
	cc.get_supported_arguments([
		'-Wno-unused-parameter',
		'-Wstrict-prototypes',
		'-Wmissing-prototypes',
		'-fvisibility=hidden',
	]),
	language: 'c'
)

foreach h: [ 'sys/prctl.h' ]
	config_h.set('HAVE_' + h.underscorify().to_upper(), cc.has_header(h))
endforeach

if not cc.has_header_symbol('drm/drm.h', 'DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE')
	error('the swapchain needs the kernel headers of Linux 6.16 or newer')
endif

foreach f: [ 'prctl', 'gettid' ]
	config_h.set('HAVE_' + f.underscorify().to_upper(), cc.has_function(f))
endforeach

configure_file(
	output: 'config.h',
	configuration: config_h,
)

# Installed poor_wayland and wayland-protocols are used if there are, the submodules otherwise
poor_wayland_options = [ 'tests=false' ]
client_dep = dependency('poor_wayland_client', fallback: 'poor_wayland', default_options: poor_wayland_options)
scanner_dep = dependency('poor_wayland_scanner', native: true, fallback: 'poor_wayland', default_options: poor_wayland_options)
scanner = find_program(scanner_dep.get_variable('poor_wayland_scanner'), native: true)
poor_loop_dep = dependency('poor_loop')
gbm_dep = dependency('gbm')
# The submodule of wayland-protocols would generate headers for libwayland with wayland-scanner if it found it
meson.override_dependency('wayland-scanner', dependency('', required: false), native: true)
wayland_protocols_dep = dependency('wayland-protocols', version: '>= 1.49', fallback: 'wayland-protocols',
	default_options: [ 'tests=false' ])
protocols_dir = wayland_protocols_dep.get_variable('pkgdatadir')

root_inc = include_directories('.')

subdir('src')

if get_option('tests')
	server_dep = dependency('poor_wayland_server', fallback: 'poor_wayland', default_options: poor_wayland_options)
	subdir('tests')
endif
if get_option('examples')
	subdir('examples')
endif
