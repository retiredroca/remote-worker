# Release guide

This file covers **releasing**. For building, adding Minecraft versions or adding modules, see
`PROJECT-GUIDE.md`.

Everything below is driven by one command, `python tools/release.py`, or by the release workflows in
`.github/workflows/`. Component versions are **declarative**: only the component you name gets a new
version; nothing infers it from the files you changed.

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

## Full local release

```bash
python tools/release.py --mod all                       # build + GitHub release, CI publishes
python tools/release.py --mod all --curseforge --modrinth   # publish from this machine instead
```

It does, in order:

1. bump `versions.properties` for the named component(s), and rewrite the dependents' library floor;
2. publish the library module(s) to the in-repo maven (`repo/`) — required before dependents compile;
3. build the staged release groups:
   `releaseLoaderJars` → `releaseLoaderBundles` → `releaseUniversal` → `releaseUniversalBundles`,
   then `releaseJars` (the ordered union) into `build/release/`;
4. copy those jars to `dist/` and write `dist/changelog.md`;
5. commit the bump/floor, push, create and push a signed tag, then create the GitHub release with
   every jar attached;
6. dispatch `.github/workflows/publish-release.yml`, which uploads the **universal** jars to
   CurseForge and Modrinth (pass `--no-ci-publish` to skip; skip it automatically with
   `--curseforge`/`--modrinth`, which publish from this machine instead).

Platform publishing needs `CURSEFORGE_API_KEY` / `MODRINTH_TOKEN` in the environment, plus
`CURSEFORGE_PROJECT_ID` / `MODRINTH_ID` when publishing locally.

## Flags

```
--mc <version>       Minecraft version folder under versions/ (default: the gradle.properties mc)
--mod <id>|all       component(s) to re-version (required)
--tag <tag>          override the auto-computed tag
--curseforge         also upload to CurseForge (needs CURSEFORGE_API_KEY, CURSEFORGE_PROJECT_ID)
--modrinth           also upload to Modrinth (needs MODRINTH_TOKEN, MODRINTH_ID)
--no-ci-publish      do not dispatch the CI publish workflow
--dry-run            print the plan; change nothing
--unsigned           do not GPG-sign the commit and tag
--allow-dirty        skip the clean-working-tree check
--no-push            commit and tag locally only
--skip-build         reuse the existing dist/ instead of rebuilding
--no-daemon          do not reuse a Gradle daemon (slower; reproduces a CI-like cold build)
--local-only         build and stage the jars locally only: no commit, tag, push, GitHub release or
                     platform/CI publishing. versions.properties is still bumped so the local jars
                     carry the next version; undo with `git checkout -- versions.properties`.
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

## Release from CI instead

Actions → **Release** → Run workflow, or comment `/release-all` on an issue/PR, choosing the
Minecraft version. CI follows the same bump/tag rules, builds the same staged groups, and then
publishes to CurseForge/Modrinth in the same run. CI cannot sign, so its tags are unsigned. See
**Enabling CI publishing** in `PROJECT-GUIDE.md` for the secrets, variables and layout.

The `release: published` trigger re-publishes an existing release through `publish-release.yml`
(used when `tools/release.py` built the release locally and asked CI to publish it).

## No `origin` remote yet?

`tools/release.py` still bumps, builds, commits and tags locally; it simply does not push or create a
GitHub release. Add a remote and re-run to publish.

## Windows note

`tools/release.py` runs the Gradle wrapper through `cmd.exe` (`cmd /c gradlew.bat`). Do not replace
that with `bash gradlew`, which resolves to Git's bash and mangles the Windows paths.
