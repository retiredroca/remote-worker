# Tools

Release helpers for this project. Both scripts are plain Python 3 (no third-party packages).

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

The only credential it reads is `GITHUB_TOKEN`, or a stored `github.com` git credential. It signs the
commit and tag with the local GPG key, so the private key never has to exist in CI.
