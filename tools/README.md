# Tools

Release helpers for this project. All five scripts are plain Python 3 (no third-party packages).
Four of them run in the build or the release pipeline; the fifth generates a texture once.

## `versioning.py`

```
python tools/versioning.py stamp                                   # the current yymmddhh stamp
python tools/versioning.py bump <version> <stamp>                  # replace the stamp component
python tools/versioning.py tag --api <version> --stamp <s>         # v<major.minor.patch>.<stamp>
python tools/versioning.py floor --api <version> [--mc <mc>]       # report a dependency floor
```

The stamp timezone is the `TIMEZONE` constant at the top of the file, set when the project was
created. It accepts an IANA zone name (`America/New_York`) or a fixed offset (`UTC`, `-10:00`,
`+05:30`). Fixed offsets always work; an IANA name needs a timezone database, which is read from
Python's `zoneinfo` or, failing that, from the JDK's `java.time` (already required to run Gradle).

## `release.py`

Bumps the version, commits, creates a **signed** tag, pushes it, and stops. The tag triggers
`release-ci.yml`, which builds the jars and one native agent binary per platform and creates the
GitHub release. See `RELEASE-GUIDE.md` for the full flow.

```bash
python tools/release.py --mod <id> --ci --dry-run
python tools/release.py --mod <id> --ci
```

The only credentials it reads are `GITHUB_TOKEN` or `GH_TOKEN`, or a stored `github.com` git
credential. It signs the commit and tag with the local GPG key, so the private key never has to exist
in CI.

## The other three

- **`protocol_vectors.py`** — the normative encoder for the wire format. It generates the byte
  vectors that both the Java and the C++ codec are checked against, so it defines the format rather
  than describing it. `PROTOCOL.md` is the prose; this is the definition. Run from `check` in both
  loader builds and from `ctest`.
- **`check_posix_includes.py`** — a text check for a POSIX-only symbol used in a file whose `#else`
  branch does not include the header declaring it. No single-platform build can see that failure; it
  broke two Linux CI legs before this existed. Run from `check`.
- **`make_tablet_texture.py`** — one-shot generator for the tablet's texture. Not part of the build
  and not run by CI; the PNG it produces is committed.
