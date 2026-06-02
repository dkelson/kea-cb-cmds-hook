cb_cmds_cpp = meson.get_compiler('cpp')
cb_cmds_feature_deps = [
    declare_dependency(include_directories: INCLUDES),
]
cb_cmds_compat_conf = configuration_data()
cb_cmds_compat_conf.set10(
    'CB_CMDS_HAVE_KEA_OPTION_CLIENT_CLASSES',
    cb_cmds_cpp.compiles(
        '''
        #include <dhcpsrv/config_backend_pool_dhcp4.h>
        void f(isc::dhcp::ConfigBackendPoolDHCPv4* pool,
               const isc::db::BackendSelector& backend,
               const isc::db::ServerSelector& server,
               isc::dhcp::ClientClassesPtr classes) {
            static_cast<void>(pool->getOption4(backend, server, 1, "dhcp4", classes));
        }
        ''',
        dependencies: cb_cmds_feature_deps,
        name: 'Kea option client-classes keys',
    ),
)
cb_cmds_compat_conf.set10(
    'CB_CMDS_HAVE_KEA_OPTION_DEF_DELETE_FORCE',
    cb_cmds_cpp.compiles(
        '''
        #include <dhcpsrv/config_backend_pool_dhcp4.h>
        void f(isc::dhcp::ConfigBackendPoolDHCPv4* pool,
               const isc::db::BackendSelector& backend,
               const isc::db::ServerSelector& server) {
            static_cast<void>(pool->deleteOptionDef4(backend, server, 1, "dhcp4", false));
        }
        ''',
        dependencies: cb_cmds_feature_deps,
        name: 'Kea option definition force delete',
    ),
)
configure_file(
    input: 'cb_cmds_compat_config.h.in',
    output: 'cb_cmds_compat_config.h',
    configuration: cb_cmds_compat_conf,
)

dhcp_cb_cmds_lib = shared_library(
    'dhcp_cb_cmds',
    'cb_cmds.cc',
    'cb_cmds_callouts.cc',
    'cb_cmds_log.cc',
    'cb_cmds_messages.cc',
    'version.cc',
    dependencies: [CRYPTO_DEP],
    include_directories: [include_directories('.')] + INCLUDES,
    install: true,
    install_dir: HOOKS_PATH,
    install_rpath: INSTALL_RPATH,
    build_rpath: BUILD_RPATH,
    link_with: LIBS_BUILT_SO_FAR,
    name_suffix: 'so',
)
dhcp_cb_cmds_archive = static_library(
    'dhcp_cb_cmds',
    objects: dhcp_cb_cmds_lib.extract_all_objects(recursive: false),
)
subdir('libloadtests')
subdir('tests')

if KEA_MSG_COMPILER.found()
    target_gen_messages = run_target(
        'src-hooks-dhcp-cb_cmds-cb_cmds_messages',
        command: [
            CD_AND_RUN,
            TOP_SOURCE_DIR,
            KEA_MSG_COMPILER,
            'src/hooks/dhcp/cb_cmds/cb_cmds_messages.mes',
        ],
    )
    TARGETS_GEN_MESSAGES += [target_gen_messages]
endif
