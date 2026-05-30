version := `make version`

# List available recipes
default:
    @just --list

# Build the static + shared libraries
build:
    make static shared

# Run the full test suite
test:
    make test-all

# Run all unit tests under AddressSanitizer + UndefinedBehaviorSanitizer
sanitize:
    #!/usr/bin/env bash
    # Catches heap/stack overflows, use-after-free, signed-integer / alignment /
    # shift UB, and memory leaks. Uses clang for the most mature ASAN/UBSAN
    # integration. Cleans first because make doesn't track CFLAGS changes.
    set -euo pipefail
    make clean >/dev/null
    make CC=clang \
      CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" \
      build/test_scanner build/test_schema build/test_emitter build/test_merge \
      build/test_resolve build/test_errors build/test_yaml_suite >/dev/null
    export ASAN_OPTIONS='symbolize=1:halt_on_error=0:detect_leaks=1:strict_string_checks=1'
    export UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=0'
    fail=0
    for t in test_scanner test_schema test_emitter test_merge test_resolve test_errors test_yaml_suite; do
        printf "── %s ──\n" "$t"
        out=$(./build/$t 2>&1)
        hits=$(printf '%s' "$out" | grep -cE 'AddressSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error' || true)
        printf '%s\n' "$out" | tail -2
        if [ "$hits" -gt 0 ]; then
            printf '  ⚠ %d sanitizer hit(s)\n' "$hits"
            fail=1
        fi
    done
    [ "$fail" -eq 0 ] && echo "all clean: 0 sanitizer hits" || { echo "sanitizer found issues"; exit 1; }

# Install the built Arch package in a clean archlinux container and smoke-test it
test-install-arch:
    #!/usr/bin/env bash
    set -euo pipefail
    ls pkg/yam-[0-9]*.pkg.tar.zst >/dev/null 2>&1 || { echo "no Arch package in pkg/ — run 'just pkg-arch' first"; exit 1; }
    podman run --rm \
      -v "$PWD/pkg:/pkg:ro" \
      -v "$PWD/test/smoke_install.c:/smoke.c:ro" \
      archlinux:base bash -c '
        set -euo pipefail
        pacman -Sy --noconfirm --needed --quiet gcc pkgconf >/dev/null
        pacman -U --noconfirm /pkg/yam-[0-9]*.pkg.tar.zst >/dev/null
        echo "── installed package ──"
        pacman -Qi yam | grep -E "^(Name|Version|Depends|Provides)" || true
        echo "── compile + run ──"
        gcc $(pkg-config --cflags yam) /smoke.c $(pkg-config --libs yam) -o /smoke
        /smoke
      '

# Install the built Debian packages in a clean debian container and smoke-test them
test-install-deb:
    #!/usr/bin/env bash
    set -euo pipefail
    ls pkg/libyam0_*.deb pkg/libyam-dev_*.deb >/dev/null 2>&1 || { echo "no Debian packages in pkg/ — run 'just pkg-deb' first"; exit 1; }
    podman run --rm \
      -v "$PWD/pkg:/pkg:ro" \
      -v "$PWD/test/smoke_install.c:/smoke.c:ro" \
      debian:stable bash -c '
        set -euo pipefail
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq gcc pkg-config libc6-dev >/dev/null
        apt-get install -y -qq /pkg/libyam0_*.deb /pkg/libyam-dev_*.deb >/dev/null
        echo "── installed packages ──"
        dpkg -l libyam0 libyam-dev | tail -3
        echo "── compile + run ──"
        gcc $(pkg-config --cflags yam) /smoke.c $(pkg-config --libs yam) -o /smoke
        /smoke
      '

# Install the built RPM packages in a clean fedora container and smoke-test them
test-install-rpm:
    #!/usr/bin/env bash
    set -euo pipefail
    ls pkg/yam-[0-9]*.rpm pkg/yam-devel-*.rpm >/dev/null 2>&1 || { echo "no RPM packages in pkg/ — run 'just pkg-rpm' first"; exit 1; }
    podman run --rm \
      -v "$PWD/pkg:/pkg:ro" \
      -v "$PWD/test/smoke_install.c:/smoke.c:ro" \
      fedora:latest bash -c '
        set -euo pipefail
        dnf install -y -q gcc pkgconf-pkg-config /pkg/yam-[0-9]*.rpm /pkg/yam-devel-*.rpm >/dev/null
        echo "── installed packages ──"
        rpm -qi yam | head -6
        echo "── compile + run ──"
        gcc $(pkg-config --cflags yam) /smoke.c $(pkg-config --libs yam) -o /smoke
        /smoke
      '

# Run all three install tests
test-install: test-install-arch test-install-deb test-install-rpm

# Fuzz the parser with libFuzzer + ASAN + UBSAN for N seconds (default 60)
fuzz duration='60':
    #!/usr/bin/env bash
    # Each crash/oom finding is written to fuzz/crashes/ for inspection;
    # libFuzzer accumulates an interesting-input corpus under fuzz/corpus/.
    # Both dirs are gitignored. Replay a finding with: ./build/fuzz_parser path
    set -euo pipefail
    mkdir -p fuzz/corpus fuzz/crashes
    make build/fuzz_parser
    ./build/fuzz_parser \
        -max_total_time={{duration}} \
        -artifact_prefix=fuzz/crashes/ \
        fuzz/corpus

# Print the version (from include/yam/yam.h)
version:
    @echo {{version}}

# Bump the version everywhere: yam.h (source of truth), PKGBUILD, the RPM spec,
# and the Debian changelog. A new dated changelog stanza is prepended for deb
# and rpm. Usage: just bump-version 0.4.0 "Summary of the release"
bump-version new_version message='New upstream release.':
    #!/usr/bin/env bash
    set -euo pipefail
    ver='{{new_version}}'
    msg='{{message}}'
    maint='Thomas Sawyer <transfire@gmail.com>'

    if [[ ! "$ver" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo "error: version must be X.Y.Z (got '$ver')" >&2
        exit 1
    fi
    IFS=. read -r major minor patch <<< "$ver"

    # 1. C header — the single source of truth for the version.
    sed -i "s/^#define YAM_VERSION_MAJOR .*/#define YAM_VERSION_MAJOR $major/" include/yam/yam.h
    sed -i "s/^#define YAM_VERSION_MINOR .*/#define YAM_VERSION_MINOR $minor/" include/yam/yam.h
    sed -i "s/^#define YAM_VERSION_PATCH .*/#define YAM_VERSION_PATCH $patch/" include/yam/yam.h

    # 2. Arch PKGBUILD (reset pkgrel to 1 for the new version).
    sed -i "s/^pkgver=.*/pkgver=$ver/" pkg/PKGBUILD
    sed -i "s/^pkgrel=.*/pkgrel=1/" pkg/PKGBUILD

    # 3. RPM spec: version, release, and a new %changelog entry on top.
    sed -i "s/^Version:.*/Version:        $ver/" pkg/yam.spec
    sed -i "s/^Release:.*/Release:        1%{?dist}/" pkg/yam.spec
    awk -v e="* $(LC_ALL=C date '+%a %b %d %Y') $maint - $ver-1\n- $msg\n" \
        '/^%changelog/ { print; print e; next } { print }' \
        pkg/yam.spec > pkg/yam.spec.tmp && mv pkg/yam.spec.tmp pkg/yam.spec

    # 4. Debian changelog: prepend a new stanza.
    { printf 'yam (%s-1) unstable; urgency=medium\n\n  * %s\n\n -- %s  %s\n\n' \
          "$ver" "$msg" "$maint" "$(LC_ALL=C date -R)"; \
      cat pkg/debian/changelog; } > pkg/debian/changelog.tmp \
      && mv pkg/debian/changelog.tmp pkg/debian/changelog

    echo "bumped yam to $ver in:"
    echo "  include/yam/yam.h  pkg/PKGBUILD  pkg/yam.spec  pkg/debian/changelog"
    echo
    echo "review the changes, then commit and tag:"
    echo "  git commit -am 'Bump version to $ver'"
    echo "  git tag v$ver && git push origin main --tags"

# Build the source tarball into pkg/
dist:
    make dist DISTDIR=pkg

# Build the Arch package with makepkg
pkg-arch: dist
    cd pkg && makepkg -f

# Build the Debian packages (libyam0 + libyam-dev) with dpkg-buildpackage
pkg-deb: dist
    rm -rf "pkg/build/yam-{{version}}"
    mkdir -p pkg/build
    tar -xzf "pkg/yam-{{version}}.tar.gz" -C pkg/build
    cp -a pkg/debian "pkg/build/yam-{{version}}/debian"
    cd "pkg/build/yam-{{version}}" && dpkg-buildpackage -us -uc -b
    mv pkg/build/*.deb pkg/build/*.ddeb pkg/ 2>/dev/null || true
    mv pkg/build/*.buildinfo pkg/build/*.changes pkg/ 2>/dev/null || true

# Build the RPM packages (yam + yam-devel) with rpmbuild
pkg-rpm: dist
    rm -rf pkg/rpmbuild
    mkdir -p pkg/rpmbuild/SOURCES pkg/rpmbuild/SPECS
    cp "pkg/yam-{{version}}.tar.gz" pkg/rpmbuild/SOURCES/
    cp pkg/yam.spec pkg/rpmbuild/SPECS/
    rpmbuild --define "_topdir $(pwd)/pkg/rpmbuild" -bb pkg/rpmbuild/SPECS/yam.spec
    find pkg/rpmbuild/RPMS -name '*.rpm' -exec cp {} pkg/ \;

# Build all three package formats
pkg-all: pkg-arch pkg-deb pkg-rpm

# Install into PREFIX (default /usr/local; override with PREFIX=...)
install:
    make install

# Remove installed files
uninstall:
    make uninstall

# Format / clean
clean:
    make clean
    rm -rf pkg/build pkg/rpmbuild pkg/pkg pkg/src
    rm -f pkg/*.tar.gz pkg/*.pkg.tar.zst pkg/*.deb pkg/*.rpm \
          pkg/*.buildinfo pkg/*.changes
