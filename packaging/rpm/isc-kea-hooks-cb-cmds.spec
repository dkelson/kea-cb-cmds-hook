%{!?cb_cmds_package_name:%global cb_cmds_package_name isc-kea-hooks-cb-cmds}
%{!?cb_cmds_version:%global cb_cmds_version 0.3.0}

Name:           %{cb_cmds_package_name}
Version:        %{cb_cmds_version}
Release:        1%{?cb_cmds_release_suffix}%{?dist}
Summary:        Configuration Backend Commands hook for Kea

License:        MPL-2.0
URL:            https://github.com/dkelson/kea-cb-cmds-hook
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  boost-devel
BuildRequires:  gcc-c++
BuildRequires:  (kea-devel or isc-kea-devel)
BuildRequires:  (kea-libs or isc-kea-mysql)
BuildRequires:  (kea-libs or isc-kea-pgsql)
BuildRequires:  libpq-devel
BuildRequires:  mariadb-connector-c-devel
BuildRequires:  meson
BuildRequires:  ninja-build
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(log4cplus)
BuildRequires:  pkgconfig(openssl)
BuildRequires:  redhat-rpm-config

%global _cb_cmds_base_optflags %{optflags}
%if 0%{?cb_cmds_x86_64_v2}
%global optflags %{_cb_cmds_base_optflags} -march=x86-64-v2 -mtune=generic
%endif

%description
Clean-room implementation of Kea's documented Configuration Backend Commands
hook. The package installs libdhcp_cb_cmds.so into Kea's hook library directory
for use by kea-dhcp4 and kea-dhcp6.

%prep
%autosetup -n %{name}-%{version}

%build
%meson \
    -Dhooksdir=%{_libdir}/kea/hooks \
    -Dkea_include_dir=%{_includedir}/kea \
    -Dkea_lib_dir=%{_libdir}
%meson_build

%install
%meson_install

%check
test -f %{buildroot}%{_libdir}/kea/hooks/libdhcp_cb_cmds.so

%files
%license LICENSE
%doc README.md NOTICE.md
%{_libdir}/kea/hooks/libdhcp_cb_cmds.so

%changelog
* Sun Jun 07 2026 Dax Kelson <daxkelson@gmail.com> - 0.3.0-1
- Harden cb_cmds multi-threading critical-section coverage.

* Tue Jun 02 2026 Dax Kelson <daxkelson@gmail.com> - 0.2.0-1
- Add ARM conformance harness and packaging release fixes.

* Tue Jun 02 2026 Dax Kelson <daxkelson@gmail.com> - 0.1.0-1
- Initial RPM packaging for the cb_cmds hook.
