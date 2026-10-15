test_runner = static_library(
	'test-runner',
	sources: [
		'test-runner.c',
		'test-helpers.c',
	],
	include_directories: root_inc,
	dependencies: [
		cc.find_library('dl', required: false),
		dependency('threads'),
		poor_base_dep,
	]
)

test_runner_dep = declare_dependency(
	link_with: test_runner,
	dependencies: [
		dependency('threads'),
		cc.find_library('dl', required: false)
	]
)

tests = {
	'swapchain-test': {
		'extra_sources': [ 'swapchain-server.c' ] + server_protocol_h,
		'deps': [ client_dep, server_dep, swapchain_dep ],
	},
	'gbm-test': {
		'deps': [ swapchain_dep ],
	},
}

# Each test runs with each ring setup
foreach test_name, test_extras : tests
	test_sources = [ test_name + '.c' ] + test_extras.get('extra_sources', [])
	test_deps = [ test_runner_dep ] + test_extras.get('deps', [])
	bin = executable(test_name, test_sources, dependencies: test_deps)
	foreach ring : [ 'default', 'coop', 'defer', 'sqpoll' ]
		test(
			test_name + '-' + ring,
			bin,
			env: [
				'TEST_SRC_DIR=@0@'.format(meson.current_source_dir()),
				'TEST_BUILD_DIR=@0@'.format(meson.current_build_dir()),
				'TEST_RING=' + ring,
			],
		)
	endforeach
endforeach

# Vulkan tests need the validation layer, which fails them on any error. Mesa's device-select layer would connect
# to the compositor of the session.
if vulkan_dep.found()
	validation_runner = find_program('python3', version: '>= 3.10', native: true)
	validation_check = files('check-validation.py')
	vulkan_env = [ 'NODEVICE_SELECT=1' ]
	test('validation-checker', validation_runner, args: files('check_validation_test.py'))
	vulkan_validation_probe = executable('vulkan-validation-probe', 'vulkan-validation-probe.c', dependencies: [ vulkan_dep, poor_base_dep ])
	test(
		'validation-reports-errors',
		validation_runner,
		args: [ validation_check, '--vulkan', vulkan_validation_probe, vulkan_validation_probe, '--report-error' ],
		env: vulkan_env,
		should_fail: true,
	)
	vulkan_test = executable(
		'vulkan-test',
		[ 'vulkan-test.c', 'swapchain-server.c' ] + server_protocol_h,
		dependencies: [ test_runner_dep, client_dep, server_dep, swapchain_dep, vulkan_dep ],
	)
	foreach ring : [ 'default', 'coop', 'defer', 'sqpoll' ]
		test(
			'vulkan-test-' + ring,
			validation_runner,
			args: [ validation_check, '--vulkan', vulkan_validation_probe, vulkan_test ],
			env: vulkan_env + [
				'TEST_SRC_DIR=@0@'.format(meson.current_source_dir()),
				'TEST_BUILD_DIR=@0@'.format(meson.current_build_dir()),
				'TEST_RING=' + ring,
			],
		)
	endforeach
endif
