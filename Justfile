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
