if not TESTS_OPT.enabled() or (not MYSQL_DEP.found() and not POSTGRESQL_DEP.found())
    subdir_done()
endif

libs_testutils = [
    kea_dhcpsrv_testutils_lib,
    kea_database_testutils_lib,
    kea_testutils_lib,
]

db_deps = [GTEST_DEP, CRYPTO_DEP]
db_libs = [dhcp_cb_cmds_archive]

if MYSQL_DEP.found()
    db_deps += [MYSQL_DEP]
    db_libs += [dhcp_mysql_archive, kea_mysql_testutils_lib]
endif

if POSTGRESQL_DEP.found()
    db_deps += [POSTGRESQL_DEP]
    db_libs += [dhcp_pgsql_archive, kea_pgsql_testutils_lib]
endif

dhcp_cb_cmds_db_tests = executable(
    'dhcp-cb-cmds-db-tests',
    'cb_cmds_db_unittest.cc',
    'run_unittests.cc',
    cpp_args: [f'-DDATABASE_SCRIPTS_DIR="@DATABASE_SCRIPTS_DIR@"'],
    dependencies: db_deps,
    include_directories: [
        include_directories('.'),
        include_directories('..'),
        include_directories('../..'),
        include_directories('../../mysql'),
        include_directories('../../pgsql'),
    ] + INCLUDES,
    link_with: db_libs + libs_testutils + LIBS_BUILT_SO_FAR,
)

test(
    'dhcp-cb-cmds-db-tests',
    dhcp_cb_cmds_db_tests,
    protocol: 'gtest',
    is_parallel: false,
    priority: -1,
    timeout: 240,
)
