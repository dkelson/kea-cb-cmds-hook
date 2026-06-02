project(
    'isc-kea-hooks-cb-cmds',
    'cpp',
    version: '0.1.0',
    license: 'MPL-2.0',
    default_options: [
        'cpp_std=c++17',
    ],
)

cpp = meson.get_compiler('cpp')

kea_dep = dependency('kea', required: false)
kea_includes = []
kea_libdir = ''
kea_version = 'unknown'

if kea_dep.found()
    kea_deps = [kea_dep]
    kea_libdir = kea_dep.get_variable(pkgconfig: 'libdir', default_value: '')
    kea_version = kea_dep.version()
else
    kea_include_dir = get_option('kea_include_dir')
    kea_libdir = get_option('kea_lib_dir')
    kea_includes = [include_directories(kea_include_dir)]

    log4cplus_dep = dependency('log4cplus', required: true)
    openssl_dep = dependency('openssl', required: true)
    kea_deps = [log4cplus_dep, openssl_dep]

    kea_lib_specs = [
        ['kea-dhcpsrv'],
        ['kea-process'],
        ['kea-config'],
        ['kea-http'],
        ['kea-tcp'],
        ['kea-dhcp', 'kea-dhcp++'],
        ['kea-hooks'],
        ['kea-database'],
        ['kea-cc'],
        ['kea-asiolink'],
        ['kea-dns', 'kea-dns++'],
        ['kea-cryptolink'],
        ['kea-log'],
        ['kea-util'],
        ['kea-util-io'],
        ['kea-exceptions'],
    ]

    foreach names : kea_lib_specs
        lib = cpp.find_library(names[0], dirs: [kea_libdir], required: false)
        foreach name : names
            candidate = cpp.find_library(name, dirs: [kea_libdir], required: false)
            if candidate.found()
                lib = candidate
                break
            endif
        endforeach
        if not lib.found()
            error('unable to find installed Kea library: ' + ' or '.join(names))
        endif
        kea_deps += [lib]
    endforeach
endif

hook_inc = include_directories('src/hooks/dhcp/cb_cmds')
kea_location_compat_inc = []
kea_location_test_deps = []
if kea_dep.found()
    kea_location_test_deps = [kea_dep.partial_dependency(compile_args: true, includes: true)]
else
    kea_location_test_deps = [declare_dependency(include_directories: kea_includes)]
endif
if not cpp.has_header('eval/location.hh', dependencies: kea_location_test_deps, required: false)
    kea_location_compat_inc = [include_directories('compat/kea-eval')]
endif
kea_feature_test_deps = kea_location_test_deps + [
    declare_dependency(include_directories: kea_location_compat_inc),
]
cb_cmds_compat_conf = configuration_data()
cb_cmds_compat_conf.set10(
    'CB_CMDS_HAVE_KEA_OPTION_CLIENT_CLASSES',
    cpp.compiles(
        '''
        #include <dhcpsrv/config_backend_pool_dhcp4.h>
        void f(isc::dhcp::ConfigBackendPoolDHCPv4* pool,
               const isc::db::BackendSelector& backend,
               const isc::db::ServerSelector& server,
               isc::dhcp::ClientClassesPtr classes) {
            static_cast<void>(pool->getOption4(backend, server, 1, "dhcp4", classes));
        }
        ''',
        dependencies: kea_feature_test_deps,
        name: 'Kea option client-classes keys',
    ),
)
cb_cmds_compat_conf.set10(
    'CB_CMDS_HAVE_KEA_OPTION_DEF_DELETE_FORCE',
    cpp.compiles(
        '''
        #include <dhcpsrv/config_backend_pool_dhcp4.h>
        void f(isc::dhcp::ConfigBackendPoolDHCPv4* pool,
               const isc::db::BackendSelector& backend,
               const isc::db::ServerSelector& server) {
            static_cast<void>(pool->deleteOptionDef4(backend, server, 1, "dhcp4", false));
        }
        ''',
        dependencies: kea_feature_test_deps,
        name: 'Kea option definition force delete',
    ),
)
configure_file(
    input: 'src/hooks/dhcp/cb_cmds/cb_cmds_compat_config.h.in',
    output: 'cb_cmds_compat_config.h',
    configuration: cb_cmds_compat_conf,
)
if kea_libdir == ''
    kea_libdir = get_option('prefix') / get_option('libdir')
endif

hooksdir = get_option('hooksdir')
if hooksdir == ''
    hooksdir = kea_libdir / 'kea/hooks'
endif

shared_library(
    'dhcp_cb_cmds',
    'src/hooks/dhcp/cb_cmds/cb_cmds.cc',
    'src/hooks/dhcp/cb_cmds/cb_cmds_callouts.cc',
    'src/hooks/dhcp/cb_cmds/cb_cmds_log.cc',
    'src/hooks/dhcp/cb_cmds/cb_cmds_messages.cc',
    'src/hooks/dhcp/cb_cmds/version.cc',
    dependencies: kea_deps,
    include_directories: [
        hook_inc,
    ] + kea_includes + kea_location_compat_inc,
    install: true,
    install_dir: hooksdir,
    name_suffix: 'so',
)

summary({
    'Kea pkg-config dependency': kea_version,
    'Hook install directory': hooksdir,
}, section: 'cb_cmds standalone build')
