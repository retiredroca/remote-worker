# Project guide

Reference for building, extending and releasing a project made from the multi-version template.
`RELEASE-GUIDE.md` covers the release flow; this file also covers everyday use, adding Minecraft
versions, and adding modules.

> **single vs multi.** A `single` project has one gameplay module and no library, so there is no
> `publishLibrary` dependency to worry about and no bundles. A `multi` project has a shared library
> plus gameplay modules, and can define bundles. Wherever this guide says "module" it applies to
> both; the library/bundle steps only apply to `multi`.

## Prerequisites

- **JDK** for Gradle. Gradle 8.14 must *run* on JDK 17–23; the project *compiles* with the
  toolchain from `versions/<mc>/version.properties` (`java_version`). If your default `java` is
  newer than 23, point `JAVA_HOME` at a JDK 21 (or set `org.gradle.java.home` in
  `gradle.properties` for the IDE only — CI strips that line).
- **Python 3** (for `tools/`). No third-party packages are needed. An IANA timezone for the version
  stamp additionally needs `tzdata`, or a JDK on `JAVA_HOME`/`PATH` (the tool falls back to
  `java.time`).
- **Git**, and `gh` only if you want CI-side publishing.

## Layout

```
versions.properties                       # per-module versions: <module-id>=<version>
bundles.properties                        # (multi) bundle.<name>=<module ids>
versions/<mc>/version.properties          # Minecraft / loader / toolchain pins (canonical)
versions/<mc>/<module>/module.properties  # module identity (id / name / group / library / depends)
versions/<mc>/<module>/common/            # loader-agnostic sources (no loader imports)
versions/<mc>/<module>/fabric/            # Fabric Loom build (relocates common into <group>.fabric.common)
versions/<mc>/<module>/neoforge/          # NeoGradle build (relocates common into <group>.neoforge.common)
gradle/loaders/{fabric,neoforge}.gradle   # shared loader build bodies
gradle/versions.gradle                    # loads versions/properties + module identity + floor helper
repo/                                     # in-repo maven for library modules (committed)
tools/versioning.py                       # stamp / bump / tag / floor
tools/release.py                          # local release driver
dist/                                     # release artifacts + changelog (gitignored, .gitkeep tracked)
```

The two loader builds are separate projects (Loom and NeoGradle cannot share one Gradle project).
Each compiles the shared `common/` sources into its own relocated package, and the build merges both
into one **universal** jar that works on either loader.

## Day-to-day

```bash
./gradlew build                  # builds the default Minecraft version (gradle.properties -> mc)
./gradlew -Pmc=1.21.1 build      # builds a specific Minecraft version
./gradlew tasks --group version  # newVersion / updateVersionProps / portChangedFiles
```

The IDE shows one Minecraft version at a time. Change `mc` in `gradle.properties` (or set `-Pmc=`
in your run configuration) and **Reload Gradle**; keep two IDE windows open to port between
versions.

### Build settings and speed

`org.gradle.jvmargs`, `org.gradle.parallel` and `org.gradle.caching` are read from a build's **own
root** `gradle.properties`. For the composite build that is this file at the project root; a nested
`gradle.properties` (e.g. `versions/<mc>/<module>/neoforge/`) is ignored when the build runs as part
of the composite, and only applies if you build that directory directly with `-p`. So keep the JVM
settings in the root file — and if you do set them per module, keep the values identical, or Gradle
starts a second daemon with the smaller heap.

The defaults are `-Xmx4G`, parallel on, caching on. For a large multi-module project raise the heap
(6–12G) and/or set `org.gradle.workers.max` (each worker is its own JVM, so keep it below your core
count rather than growing the daemon heap).

### Build the release groups by hand

The release build is four ordered groups, each writing its own directory; `releaseJars` unions them
into `build/release/`:

```bash
./gradlew -Pmc=<mc> releaseLoaderJars        # build/release-loader/          (per-loader jars)
./gradlew -Pmc=<mc> releaseLoaderBundles     # build/release-loader-bundles/  (per-loader bundles, multi)
./gradlew -Pmc=<mc> releaseUniversal         # build/release-universal/       (merged universal jars)
./gradlew -Pmc=<mc> releaseUniversalBundles  # build/release-universal-bundles/ (universal bundles, multi)
./gradlew -Pmc=<mc> releaseJars              # union of all four -> build/release/
```

Groups 1 and 2 accept `-Ploader=fabric|neoforge` to build one loader only. `cleanLoader` and
`cleanUniversal` wipe a pair of group directories so a partial rebuild cannot leave a stale jar.

### Library modules (multi)

Modules with `library=true` are published to the in-repo maven (`repo/`), and dependent modules
resolve them as a floor range `[<major.minor.patch>,<upper>)` computed from `versions.properties`
at configuration time. Because that resolution happens while Gradle configures, **the library must
be in `repo/` before its dependents compile**:

```bash
./gradlew -Pmc=<mc> publishLibrary     # all library modules, both loaders
```

If `publishLibrary` itself cannot configure (a brand-new Minecraft version), publish a library
directly, then build:

```bash
./gradlew -Pmc=<mc> -p versions/<mc>/<library>/fabric publishMavenJavaPublicationToRepoRepository
./gradlew -Pmc=<mc> -p versions/<mc>/<library>/neoforge publishMavenJavaPublicationToRepoRepository
./gradlew -Pmc=<mc> build
```

`tools/release.py` and CI do this as their own step, so an ordinary release never hits the problem.

## Adding a Minecraft version

```bash
./gradlew newVersion -Pnew=1.21.11
```

This copies `versions/1.21.1` to the new directory and resolves Fabric Loader / Fabric API /
NeoForge for it, writing `versions/<mc>/version.properties` (the canonical pins). Re-resolve later
with `./gradlew updateVersionProps -Pnew=<mc>`.

Then publish each library for the new version and build:

```bash
./gradlew -Pmc=1.21.11 publishLibrary
./gradlew -Pmc=1.21.11 build
```

Port your changes across versions with:

```bash
./gradlew portChangedFiles -Pfrom=1.21.1 -Pto=1.21.11 [-Psince=<ref>]
```

…then fix the API differences. A new Minecraft version is only *declared* here until it is built;
nothing else needs to know about it.

> **Toolchain generations.** 1.21.x and 26.x are *not* the same generation: 26.1+ needs Java 25, is
> deobfuscated, and uses a renamed Loom plugin. `version.properties` carries a `toolchain_era` used
> by the version resolver; a cross-generation port needs the source changes too, not just the pins.

## Adding a module (multi)

1. Copy an existing module directory under `versions/<mc>/` (e.g. `cp -r mod2 mod3`).
2. Edit the new `versions/<mc>/<module>/module.properties`:

   ```properties
   id=mod3
   name=My Third Mod
   group=com.retiredroca.remoteworker
   authors=Retired Roca
   license=Apache-2.0
   description=...
   library=false
   depends=api          # comma-separated ids this module needs
   ```

3. Rename the package directories and update package declarations to the new module's package.
4. Add a line to `versions.properties` (`mod3=1.0.0`).
5. Optionally add a bundle to `bundles.properties` (`bundle.mod3=api,mod3`); container metadata is
   generated by the build, so no extra files are needed.
6. Publish the library and build: `./gradlew -Pmc=<mc> publishLibrary build`.

Because modules are discovered by scanning `versions/<mc>/`, no registration is required anywhere
else. The release jar count (`tools/release.py`, CI) is computed from the module and bundle lists,
so a new module is picked up automatically.

## Adding a bundle (multi)

Add a line to `bundles.properties`:

```properties
bundle.all=api,mod1,mod2
bundle.lite=api,mod1
```

`releaseUniversalBundles` / `releaseLoaderBundles` pick it up on the next build. A bundle nests the
per-loader jars of its modules and generates `fabric.mod.json` / `neoforge.mods.toml` / jarjar
metadata, so a listing is all that is needed.

## Release

This project does not publish to CurseForge or Modrinth. It has one release surface: the GitHub
release, built by `.github/workflows/release-ci.yml` from a tag you sign locally. See the **Releases**
section of `README.md` for the full flow, and `README.md` for why it differs from the template.

```bash
python tools/release.py --mod <id> --ci --dry-run   # preview, changes nothing
python tools/release.py --mod <id> --ci            # signed tag; CI builds and releases it
```

Nothing needs configuring: the workflow needs no secrets beyond the automatic `GITHUB_TOKEN`, and no
repository variables.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `Could not find ... :api-fabric-<mc>:[1.0.0,1.1)` | The library is not published yet. Run `./gradlew -Pmc=<mc> publishLibrary`, or the two direct `publishMavenJavaPublicationToRepoRepository` invocations. |
| Gradle fails with `Unsupported class file major version 69` | Gradle is running on too-new a JDK. Point `JAVA_HOME` at a JDK 17–23. |
| `cannot resolve timezone` from `tools/versioning.py` | The configured `TIMEZONE` is an IANA name and no tzdata/JDK is available. Install `tzdata` (`pip install tzdata`) or use a fixed offset such as `-10:00`. |
| Release says "release jar set mismatch" | A group build failed or a stale jar is present. Run the four groups in order, or `cleanLoader` + `cleanUniversal`, then retry. |
| `git add` / release aborts on a dirty tree | Commit or stash first, or pass `--allow-dirty`. |
| Windows: `gradlew` not found from `release.py` | `tools/release.py` invokes `cmd /c gradlew.bat` on Windows; do not change it to `bash gradlew`, which resolves to Git's bash and mangles paths. |
