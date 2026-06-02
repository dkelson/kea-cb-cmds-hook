if not TESTS_OPT.enabled()
    subdir_done()
endif

dhcp_cb_cmds_tests = executable(
    'dhcp-cb-cmds-tests',
    'cb_cmds_unittest.cc',
    'run_unittests.cc',
    cpp_args: [f'-DTOP_SOURCE_DIR="@TOP_SOURCE_DIR@"'],
    dependencies: [GTEST_DEP, CRYPTO_DEP],
    include_directories: [include_directories('.'), include_directories('..')] + INCLUDES,
    link_with: [
        dhcp_cb_cmds_archive,
        kea_dhcpsrv_testutils_lib,
        kea_dhcp_testutils_lib,
        kea_database_testutils_lib,
        kea_testutils_lib,
    ] + LIBS_BUILT_SO_FAR,
)
test('dhcp-cb-cmds-tests', dhcp_cb_cmds_tests, protocol: 'gtest')

test(
    'dhcp-cb-cmds-api-coverage',
    PYTHON,
    args: [
        files('api_coverage.py'),
        '--api-dir', f'@TOP_SOURCE_DIR@/src/share/api',
        '--unit-source', f'@TOP_SOURCE_DIR@/src/hooks/dhcp/cb_cmds/tests/cb_cmds_unittest.cc',
        '--db-source', f'@TOP_SOURCE_DIR@/src/hooks/dhcp/cb_cmds/dbtests/cb_cmds_db_unittest.cc',
    ],
)
