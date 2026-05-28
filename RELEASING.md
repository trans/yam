# Releasing yam

The version lives in `include/yam/yam.h` (the `YAM_VERSION_MAJOR`/`MINOR`/`PATCH`
macros) and is the single source of truth. Everything else — the Arch
`PKGBUILD`, the RPM spec, the Debian changelog, the dist tarball name, the
`pkg-config` `Version:`, the SONAME — derives from it. A release is just:

1. **bump the version** (one command, all four files updated),
2. **commit + push** to `main`,
3. **tag** `vX.Y.Z` and push the tag,
4. **publish a GitHub release** for that tag.

The `Package` workflow does the rest: it builds Arch, Debian, and RPM packages
in their native build environments and attaches them to the release.

## Cutting a release

### 1. Pick a version

See [Versioning](#versioning) below. The current version is whatever
`make version` (or `just version`) prints.

### 2. Bump the version everywhere

```sh
just bump-version 0.4.0 "Short summary of the release"
```

The recipe updates:

- `include/yam/yam.h` — splits `0.4.0` into the three version macros.
- `pkg/PKGBUILD` — sets `pkgver`, resets `pkgrel=1`.
- `pkg/yam.spec` — sets `Version:`, resets `Release:`, prepends a dated
  `%changelog` entry with the summary message.
- `pkg/debian/changelog` — prepends a new `yam (X.Y.Z-1) unstable; …` stanza
  with the summary message.

The summary message is the bullet that goes into the deb and rpm changelogs.
It defaults to `"New upstream release."` if you omit it.

Review the changes (`git diff`) — especially the prepended changelog entries,
which you may want to expand into multiple bullets before committing.

### 3. Commit and push

```sh
git commit -am "Bump version to 0.4.0"
git push origin main
```

### 4. Tag and push the tag

```sh
git tag v0.4.0
git push origin v0.4.0
```

The tag format must be `vX.Y.Z` — the workflow's version-check rejects any
other shape.

### 5. Publish the GitHub release

```sh
gh release create v0.4.0 --title "v0.4.0" --notes "$(cat <<EOF
Short paragraph or bullet list of what's in this release.

Refer to debian/changelog or %changelog in the spec for the per-package
changelog entries, which are also visible to package users via
\`apt changelog libyam0\` and \`rpm -q --changelog yam\`.
EOF
)"
```

Creating the release fires the `release: published` event, which triggers
the `Package` workflow. It takes about 90 seconds end-to-end; you can watch
it with `gh run watch`. When it finishes, the release page has the package
assets attached.

## Versioning

Standard semver. The version macros in `yam.h` are the only place where the
version is "real"; everything else is downstream.

| Bump  | When | Example |
|-------|------|---------|
| **PATCH** | Bug fixes, packaging changes, internal refactors that don't change observable behavior or the public API. | 0.3.0 → 0.3.1 (packaging release) |
| **MINOR** | New public API or features, backward-compatible. | 0.3.1 → 0.4.0 (new emitter mode) |
| **MAJOR** | Backward-incompatible API or ABI changes. | 0.4.x → 1.0.0 (stable API) |

**ABI / SONAME.** The shared library's SONAME is `libyam.so.$(SOVERSION)`,
where `SOVERSION` is `YAM_VERSION_MAJOR` (see the Makefile). So throughout
the `0.x` series the SONAME stays `libyam.so.0`, regardless of MINOR/PATCH
bumps. The practical implication: the `0.x` series carries **no ABI
stability promise**. Downstream consumers should rebuild against a new 0.x
release; an existing `libyam.so.0` binary may or may not keep working
depending on what changed. This matches what's conventional for pre-1.0
libraries.

When 1.0 ships, the SONAME becomes `libyam.so.1` and the rules tighten:
any ABI break requires a MAJOR bump (and thus a SONAME bump), and patch /
minor releases must preserve ABI. If you want stronger guarantees inside
`0.x`, decouple `SOVERSION` from `VERSION_MAJOR` in the Makefile and bump
it by hand on ABI breaks — but that's an explicit choice.

What counts as an ABI break:

- changing a struct's layout (size, field order, field type),
- changing or removing an exported function's signature,
- removing an exported symbol,
- changing the meaning of an existing enum value.

Adding new functions, new enum values at the end, or new fields at the end of
opaque/forward-declared structs is ABI-safe.

## What the workflow does

`.github/workflows/package.yml` defines four jobs, gated on `release:
published` (the auto-build path) and `workflow_dispatch` (manual dry-run path).

| Job | Builder | Output |
|-----|---------|--------|
| **arch** | `archlinux:base-devel` container, `makepkg` | `yam-VERSION-1-x86_64.pkg.tar.zst` (+ auto-split `yam-debug-*`) |
| **deb** | `ubuntu-latest`, `dpkg-buildpackage -b` | `libyam0_VERSION-1_amd64.deb`, `libyam-dev_VERSION-1_amd64.deb` |
| **rpm** | `fedora:latest` container, `rpmbuild -bb` | `yam-VERSION-1.fcXX.x86_64.rpm`, `yam-devel-*.rpm` (+ `debuginfo`/`debugsource`) |
| **release-assets** | `ubuntu-latest` | Downloads the above and attaches them to the release |

Before building, each of the three build jobs verifies that
`github.event.release.tag_name` equals `v$(make version)`. This catches a
mismatched tag (e.g. tagging `v0.4.0` but forgetting to bump `yam.h`) before
any build runs. The `release-assets` job is gated on
`github.event_name == 'release'`, so dispatch runs skip it.

The Arch and RPM jobs each run the non-suite unit tests as part of their
build (`check()` / `%check`) — schema, scanner, emitter, merge, resolve,
errors. The YAML Test Suite is excluded because it lives in a submodule
that's intentionally not in the release tarball.

## Dry runs

To exercise the build jobs without publishing anything — useful when you've
touched any of the packaging files and want to confirm nothing's broken
before tagging:

```sh
gh workflow run package.yml --ref main
gh run watch
```

The three build jobs run; `release-assets` skips. Each job uploads its
package as a **workflow artifact** (visible on the run page or via
`gh run download <run-id>`), not as a release asset.

This is the right way to test changes to: `PKGBUILD`, `pkg/debian/*`,
`pkg/yam.spec`, the workflow itself, or the `Makefile`'s install / dist /
shared-library logic.

## Recovering from a bad release

If you publish a release and discover a packaging bug, the cleanest fix is
to bump a patch version and cut a new release — published releases are
public, and consumers may have already cached the assets.

If you must redo the same version (e.g. the release literally never built
any assets and nobody has seen it):

```sh
gh release delete vX.Y.Z --cleanup-tag --yes   # removes release + remote tag
git tag -d vX.Y.Z                              # remove local tag
# fix the bug, then re-run from step 4 (tag + release).
```

If only some asset uploads failed (e.g. one job errored), you can re-run the
failed jobs from the Actions UI or with `gh run rerun <run-id> --failed`;
the upload step is idempotent (it overwrites assets on the existing
release).
