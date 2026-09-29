# Release guide

This file covers **releasing**. For building, adding Minecraft versions or adding modules, see
`PROJECT-GUIDE.md`.

Everything below is driven by one command, `python tools/release.py`, and by
`.github/workflows/release-ci.yml`. Component versions are **declarative**: only the component you
name gets a new version; nothing infers it from the files you changed.

The local command no longer builds the release. It bumps the version, commits, creates a **signed**
tag, pushes it, and stops; the workflow triggered by that tag does the building. That is not a
stylistic choice — a native agent binary only exists for the platform it was compiled on, so a
release built on one machine cannot produce a complete release. See the **Releases** section of
`README.md`.

## Version scheme

```
<major>.<minor>.<patch>.<yymmddhh>        e.g. 1.0.2.26091512
v<major>.<minor>.<patch>.<yymmddhh>        e.g. v1.0.2.26091512   (the tag)
```

* the trailing `yymmddhh` stamp is bumped automatically, in the timezone this project was created
  with (see `TIMEZONE` at the top of `tools/versioning.py`);
* the patch (3rd component) is edited by hand when a semantic bump is wanted;
* two releases in the same hour produce the same version/tag — space them out or pass `--tag`.

Dependent modules ask for a library as a **floor range** `[<major.minor.patch>,<upper>)`, so an older
library can never silently satisfy a newer dependent. `tools/release.py` rewrites that floor for you
when the library is the component being released.

## Which `--mod` to use

```
--mod <module id>     a single component (see versions.properties)
--mod all             every component
```

Use `--mod all` whenever a change spans the library and one or more dependents (for example moving
shared code into the library). A narrow release leaves the other components at their old versions,
and the bundles then mix old and new.

## Preview

```bash
python tools/release.py --mod all --dry-run
```

`--dry-run` prints every step without changing any file or touching git.

## The release

```bash
python tools/release.py --mod all --ci
```

It does, in order:

1. bump `versions.properties` for the named component(s), and rewrite the dependents' library floor;
2. commit the bump/floor and push it;
3. create a **signed** tag and push it;
4. stop.

The tag push is the hand-off. `release-ci.yml` then builds the mod jars on ubuntu (Java only), builds
and `ctest`s the agent natively on four runners — `ubuntu-latest` / `windows-latest` for x86_64 and
`ubuntu-24.04-arm` / `windows-11-arm` for arm64 — and, only if every one of those passed and the
assembled set is exactly one binary per platform, creates the GitHub release with all seven files
attached. A platform that fails stops the release rather than shipping a partial set.

Nothing else is published anywhere. This project has no CurseForge or Modrinth presence; that came
from the template it was generated from, together with three workflows and a set of flags that
existed only to drive it, all of which have been removed.

There is also a fallback, kept for emergencies: running **without** `--ci` builds and publishes the
release from this machine. It can only ever attach the *host* platform's agent binary, so a release
made that way is incomplete for anyone else.

## Flags

```
--mc <version>       Minecraft version folder under versions/ (default: the gradle.properties mc)
--mod <id>|all       component(s) to re-version (required)
--tag <tag>          override the auto-computed tag
--ci                 bump, commit, sign and push a tag, then stop; the release workflow builds it
--dry-run            print the plan; change nothing
--unsigned           do not GPG-sign the commit and tag
--allow-dirty        skip the clean-working-tree check
--no-push            commit and tag locally only
--skip-build         reuse the existing dist/ instead of rebuilding
--no-daemon          do not reuse a Gradle daemon (slower; reproduces a CI-like cold build)
--local-only         build and stage the jars locally only: no commit, tag, push or GitHub release.
                     versions.properties is still bumped so the local jars carry the next version;
                     undo with `git checkout -- versions.properties`.
```

### Testing a change without publishing

To build jars you can drop into a `mods/` folder and check in-game, without committing, tagging or
publishing anything:

```bash
python tools/release.py --mod <id> --local-only --allow-dirty
```

The jars land in `build/release/` and `dist/` (named with the next version stamp). When you are done
testing, `git checkout -- versions.properties` to undo the bump. A `--local-only` run leaves no
commit, tag or release behind.

A full release stops the Gradle daemon when it finishes, because an idle daemon keeps Loom's cached
mapping jars open and the next build then fails with `FileSystemException: ... being used by another
process`. If you run Gradle by hand and hit that error, `./gradlew --stop` releases the lock. The
daemon is reused between the invocations of a single release, so the cost is one cold start per run.

## Retrying a release

Actions → **Release** → Run workflow, entering the **existing** tag. This rebuilds and republishes
that version without bumping it, which is how a failed run is retried without spending a new version
number. The workflow deletes and re-uploads the release assets it owns, so a retry is not additive.

Note that the tag is the only signed part. The version-bump commit and the tag are both made on the
maintainer's machine precisely so the private key never has to exist in CI.

## No `origin` remote yet?

`tools/release.py` still bumps, builds, commits and tags locally; it simply does not push or create a
GitHub release. Add a remote and re-run to publish.

## Windows note

`tools/release.py` runs the Gradle wrapper through `cmd.exe` (`cmd /c gradlew.bat`). Do not replace
that with `bash gradlew`, which resolves to Git's bash and mangles the Windows paths.
