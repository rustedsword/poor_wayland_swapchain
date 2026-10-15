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
