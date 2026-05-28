Name:           yam
Version:        0.3.1
Release:        1%{?dist}
Summary:        Fast, minimal, zero-copy YAML 1.2 parser and emitter library in C11

License:        MIT
URL:            https://github.com/trans/yam
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make

%description
yam is a fast, minimal, zero-copy YAML 1.2 parser and emitter written in C11.
It features a SIMD-accelerated scanner selected at runtime, an event-based
parser, an emitter with block/flow/minimal output styles, merge key expansion,
alias resolution, structured error messages, and an arena allocator.

This package contains the shared library.

%package devel
Summary:        Development files for yam
Requires:       %{name}%{?_isa} = %{version}-%{release}

%description devel
The yam development files: C headers, the static library, the development
symlink, and the pkg-config file needed to build against yam.

%prep
%autosetup

%build
# SIMD is selected at runtime, so the portable distro flags (%%{optflags}, no
# -march=native) still produce a binary that uses SIMD on capable CPUs.
%make_build static shared CFLAGS="%{optflags}" LDFLAGS="%{?build_ldflags}"

%install
%make_install PREFIX=%{_prefix} LIBDIR=%{_libdir}

%check
make test test-schema test-emitter test-merge test-resolve test-errors \
    CFLAGS="%{optflags}"

%ldconfig_scriptlets

%files
%license LICENSE
%doc README.md
# Versioned shared library + SONAME symlink (the bare .so goes in -devel).
%{_libdir}/libyam.so.*

%files devel
%dir %{_includedir}/yam
%{_includedir}/yam/*.h
%{_libdir}/libyam.so
%{_libdir}/libyam.a
%{_libdir}/pkgconfig/yam.pc

%changelog
* Wed May 27 2026 Thomas Sawyer <transfire@gmail.com> - 0.3.1-1
- Add Arch, Debian, and RPM packaging; select the SIMD scanner path at runtime

* Wed May 27 2026 Thomas Sawyer <transfire@gmail.com> - 0.3.0-1
- Initial RPM packaging (yam shared library + yam-devel development files).
- SSE4.2 SIMD scanner path is selected at runtime, so the portable build uses
  SIMD on capable CPUs without requiring -march=native.
