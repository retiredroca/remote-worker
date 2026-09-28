# Gotchas — platform and project specifics

Facts, not rules. Everything here is true of **this** machine and **this** project, and will go stale.
The general working rules live in `AGENTS.md`, the layout in `PROJECT-GUIDE.md`, and the release
workflow in `RELEASE-GUIDE.md`.

Each entry earns its place by costing time. **If you discover one, write it down here yourself — do not
wait to be asked, and do not write it retrospectively.** Include the symptom as observed, the check
that proves it, the cause with `file:line`, and the fix. An entry holding only a conclusion is nearly
worthless, because the next reader cannot tell whether it still applies. Delete the ones that stop
holding; a stale gotcha is worse than none, because it gets trusted.

## Toolchain

### Finding the required JDK, rather than guessing one

The Java version a Minecraft version needs is usually already installed on the machine by the launcher
you use to play, and the launcher records which runtime each instance runs on. Look there before
installing anything or trusting a remembered path.

- **Minecraft launchers keep their own Java installs.** For Prism Launcher / MultiMC and the Modrinth
  App that is an `appdata`/config directory beside the launcher's other data — a `java` folder holding
  one directory per installed runtime — and some builds instead place a `runtime` directory inside each
  instance. The official Mojang launcher ships exactly one runtime, under the install directory. The
  launcher's own *Java installations* / *Settings -> Java* screen lists the same installs and can add
  more.
- **Do not trust the directory name to tell you the version.** Launchers commonly name runtime
  directories by an opaque or obfuscated identifier rather than by version number, so `java-runtime-x`
  may be 17, 21 or 25. Resolve it by running the binary:

  ```bash
  "<path-to-runtime>/bin/java" -version
  ```

  Use `bin/java` rather than `bin/javaw`: `javaw` is the windowless variant the launcher points a game
  at, so the game does not open a console.
- **The instance is the authority.** A launcher records the chosen runtime per instance, in a settings
  or `instance.cfg` file, as both a `JavaPath` and a `JavaVersion`. Reading that tells you exactly what
  a given Minecraft version runs on, with no inference.
- **To obtain a version you do not have,** make an instance for a Minecraft version that requires it —
  the launcher downloads the matching runtime with the instance — then read the path back out of that
  instance's settings.
- Point the build at whatever you find through the environment variable the build tool reads
  (`JAVA_HOME` for Gradle, or the IDE's own runtime setting), not by editing build files.

- Gradle must run on JDK 17–23. If the machine default is newer, point `JAVA_HOME` at a JDK 21.
  The machine default here is **25.0.1**, which Gradle 8.14 rejects. The JDK 21 that works is the one
  Gradle itself provisioned — `C:\Users\Ric\.gradle\jdks\eclipse_adoptium-21-amd64-windows.2` — which
  is also what the warm daemon runs on. The Prism Launcher runtimes are the other source;
  `java-runtime-delta` is the 21.0.3 one (`epsilon` is 25, `gamma` is 17).
- Select the Minecraft version with `-Pmc=<version>` (default from `gradle.properties`).
- `JVM settings live in the root gradle.properties only`: `org.gradle.jvmargs` is read from a build's own
  root `gradle.properties`, so nested per-module and per-loader files repeat the same value
  deliberately. If they disagree, Gradle starts a second daemon with the wrong heap. Do not raise the
  daemon heap to fix a worker OOM — each worker is its own JVM, so `org.gradle.workers.max` is the lever,
  and it is kept below the core count.
- Do not pass `--no-daemon` locally. Each invocation cold-starts and reconfigures every included build.
  `tools/release.py` reuses the warm daemon and exposes `--no-daemon` to reproduce a CI-like cold build;
  CI always passes it (fresh container).

## Build & release

- Build the staged release groups, then the union (single-mod project: no bundles, so those groups stay
  empty but keep the uniform contract):

  ```bash
  ./gradlew -Pmc=<mc> releaseLoaderJars
  ./gradlew -Pmc=<mc> releaseLoaderBundles
  ./gradlew -Pmc=<mc> releaseUniversal
  ./gradlew -Pmc=<mc> releaseUniversalBundles releaseJars
  ```

  `build/release/` must then hold the complete set (jars × loaders + universal jars + bundle variants).
- The library module(s) must be published before dependents compile, because dependents resolve a
  version floor from the local repository:

  ```bash
  ./gradlew -Pmc=<mc> publishLibrary
  ```

  CI and `tools/release.py` publish the library as their own step.

## Layout & conventions

- Anything shared belongs in the library module (`module.properties` with `library=true`), never
  duplicated per loader or per gameplay module.
- Per-loader layout: `common` + `fabric` + `neoforge` included builds, with the shared sources relocated
  per loader so one universal jar can hold both mapping variants.
- Versions use the `<major>.<minor>.<patch>.<yymmddhh>` scheme (`tools/versioning.py`); release tags are
  `v<major>.<minor>.<patch>.<stamp>`.
- Version bumps are **declarative**: only the component named by `--mod` is re-versioned, and nothing
  infers it from which files were touched. When a change spans the library and one or more dependents,
  release with `all` so the bundles cannot mix new and old component versions.

## Gotchas

### A rename changes where names are built, not where they are read

- **Symptom:** after renaming release jars, the build is green, every jar is produced, the count is
  right, and the tests pass -- but CurseForge/Modrinth receive nothing, or receive a truncated
  version. No step reports an error. In this project `--curseforge` published **zero** of five
  bundles and logged `done`.

- **Check that proves it:** take the names actually in `build/release/` and make every consumer
  resolve against them. For each of these, a wrong answer is a silent no-op:
  - every `glob()`/shell glob, and what `sorted()[0]` actually selects (3 files match, not 1)
  - every version-extraction `sed`/slice: print the extracted string and compare to the real version
  - every constructed path: assert `(dist / name).exists()` for all of them
  - the workflow's own `allowed=` regex, run against the real names
  A count check cannot catch this: 30 stale jars and 30 current jars are both 30.

- **Cause:** the rename updated the code that *builds* a name and left the code that *reads* one.
  `bundle-<key>-*.jar` matched 3 files (fabric/neoforge/universal) where
  `universal-bundle-<key>.*.jar` had matched 1, so `sorted()[0]` changed meaning silently, and a
  `-universal.jar` suffix slice then ate 3 characters of the version. Every `if jar.exists():`
  turned each wrong path into a quiet skip. Both `dist/` folders held only pre-rename jars, so the
  stale lines looked plausible on inspection.

- **Fix:** one name per artifact built by one helper, and a glob that names the exact file rather
  than a directory plus `[0]`. Replace every `if jar.exists(): continue` on an upload path with
  `die(...)` -- a missing file there is a bug, not a reason to publish a partial set. Then add the
  shape guard: `ALLOWED_JAR` in `tools/release.py` and `allowed=` in the workflow, both
  `^[A-Za-z0-9_-]+-[0-9][0-9.]*-(fabric|neoforge|universal)\.jar$`. Dashes are required in the id
  group for a `bundle-` prefix. Clear `dist/` after a rename so nothing can resolve a stale file.

- **When auditing a rename, grep for the *old* shape, not for a word like `universal`.** A pattern
  of `universal[-_.]` also matches the new correct `...-universal.jar`, so the check reports hits
  that are fine, and reading those as failures (or as "clean") is how a stale line survives review.

Add project- and platform-specific traps here as they are found, under a heading that says what the
symptom looked like. Keep the check that proves it.

### The package directory is named with a Nerd Font glyph in it, and nothing has ever compiled

- **Symptom:** `create-project` made
  `versions/1.21.1/remote-worker/common/src/main/java/com/retiredroca/remotewerk<U+F01B>[D...` and
  every `package` line read `package com.retiredroca.remoteworker<ESC>[D<ESC>[C;`. The project was a
  pristine template output with **zero commits**, so the damage was invisible until it was built.

- **Check that proves it:** the two spellings are *different characters*, which is why this is easy
  to misread. In the **file contents** it is a real `0x1B` ESC byte; in the **directory name** the
  ESC has been replaced by `U+F01B`, because `0x1B` is an illegal Win32 filename character. Check
  each separately:

  ```bash
  grep -rlP '\x1b' --include=*.java --include=*.json --include=*.properties versions/   # contents
  find versions -type d -path '*java*' -name 'retiredroca*'                             # directory names
  ```

  To prove it cannot compile without involving Minecraft, compile the loader-free sources alone —
  they reference nothing outside the mod, so no classpath is needed:

  ```bash
  find versions/1.21.1/remote-worker/common/src/main/java -name '*.java' > /tmp/srcs.txt
  "$JAVA_HOME/bin/javac" -d /tmp/javac-out @/tmp/srcs.txt
  ```

- **Cause:** `create-project.sh:65` and `create-project.ps1:58` take the group verbatim from `read -r`
  / `Read-Host` with no validation, and the one string is then used as **two incompatible things**:
  a Java package (`create-project.sh:169`, `create-project.ps1:124`) and a filesystem path
  (`create-project.sh:177` builds `GROUP_PATH` by replacing `.` with `/`; `create-project.ps1:149`
  replaces `.` with `\`). Those character sets cannot both be satisfied — measured on the JDK,
  `isJavaIdentifierPart('[')` is `false`, and `Path.GetInvalidFileNameChars()` contains `0x1B`. So
  any value carrying a control character or a bracket is legal in exactly one of the two positions.
  `<ESC>[D` and `<ESC>[C` are *cursor-back* and *cursor-forward*, which is what a shell prompt emits
  while redrawing part of a line, so a paste from a rendered terminal line is how they arrive; `read`
  captures pasted bytes with no filtering. The failure is silent because the file contents are
  rewritten (`.sh:144-169`, `.ps1:131-140`) **before** the directory move (`.sh:176-185`,
  `.ps1:148-160`), and the cleanup after it is `rmdir ... || true` / `-ErrorAction SilentlyContinue`.

- **Fix:** recreate the project passing `--group`/`-Group` explicitly rather than accepting the
  prompt, then build once before trusting it. Do not patch the package in place by hand: the same
  value has to be corrected in the file contents *and* the directory names, and they do not contain
  the same character.

### `create-project.ps1` writes `package com\.foo\.bar;` into every source

- **Symptom:** any project created with the **PowerShell** script fails to compile on
  `package com\.example\.mymod;` — backslashes in the package declaration. The bash script's
  projects are unaffected, so this only shows up if `.ps1` is the path actually used.

- **Check that proves it:** run the substitution in isolation; no project needed:

  ```powershell
  [System.Text.RegularExpressions.Regex]::Replace('package com.example.examplemod;',
      'com\.example\.examplemod',
      [System.Text.RegularExpressions.Regex]::Escape('com.foo.bar'))
  ```

  It returns `package com\.foo\.bar;`. A correct run returns `package com.foo.bar;`.

- **Cause:** `create-project.ps1:137` passes `Regex::Escape(...)` as the **replacement** argument.
  `Regex::Escape` escapes a value for use as a *pattern*; a replacement string is not a pattern, so
  the added backslashes are inserted literally. Any group with two or more segments is affected,
  because the dots are what get escaped. `create-project.sh:142` gets this right — its `esc()`
  escapes for the right-hand side of a `sed` substitution, which is the analogous position.

- **Fix:** on Windows prefer `create-project.sh` (from Git Bash) until the `.ps1` is corrected, or
  apply the fix to the template: the replacement needs no escaping for `.` at all, and anything that
  does need it must be escaped with `Regex.Replace(input, pattern, evaluator)` or by doubling `$`.

### `rm -rf` on the project directory fails with "Device or resource busy"

- **Symptom:** deleting the project folder reports `Device or resource busy` and leaves it in place,
  even after moving every shell's working directory out of it and stopping the Gradle daemon.

- **Check that proves it:** stop the daemon, then retry — it still fails, and `jps -l` shows no
  Gradle daemon left:

  ```bash
  "$JAVA_HOME/bin/jps.exe" -l | grep -i gradle
  rmdir versions-check-dir
  ```

  The remaining holder is the agent session itself: its workspace root *is* the directory, so it
  holds a handle that nothing inside the session can release.

- **Cause:** two independent holders, easily confused because they produce the identical message.
  A warm Gradle daemon keeps the composite build's directories open (it was running with
  `-Xmx12G` and had to be stopped with `./gradlew --stop` from *another* project, since this one's
  `gradlew` had already been deleted). After that, the editor/agent process holding the workspace
  root is still the blocker.

- **Fix:** stop the daemon from a sibling checkout, then do not fight the directory handle — create
  the project beside it and copy the contents across, which is equivalent because `create-project`
  only ever writes into a non-existent directory:

  ```bash
  ./create-project.sh --mode single --dir remote-worker-new --group com.retiredroca.remoteworker ...
  cp -a remote-worker-new/. remote-worker/ && rm -rf remote-worker-new
  ```

  `cp -a src/. dst/` is the form that brings `.git`, `.github` and `.gitignore` across; `cp -a src/*`
  silently skips every dotfile.

### A common test cannot be launched by its source name — the sources are relocated

- **Symptom:** a test under `common/src/test/java` compiles, and running it by its own fully
  qualified name gives `ClassNotFoundException` — while the class file plainly exists under
  `build/classes/java/test/`.

- **Check that proves it:** the class file's path is not the package in the source. Compare them:

  ```bash
  find versions/1.21.1/remote-worker/fabric/build/classes/java/test -name '*.class'
  grep -m1 '^package' versions/1.21.1/remote-worker/common/src/test/java/com/retiredroca/\
  remoteworker/protocol/ProtocolVectorsCheck.java
  ```

  The build prints `.../remoteworker/fabric/common/protocol/ProtocolVectorsCheck.class` against
  `package com.retiredroca.remoteworker.protocol;`.

- **Cause:** `main` sources are relocated per loader so one universal jar can hold both mapping
  variants (see the `relocateCommonSources` task in
  `versions/1.21.1/remote-worker/fabric/build.gradle`), and `main.java.srcDirs` is *replaced* rather
  than extended, so `common/src/test/java` is not compiled until the same relocation is applied to
  the test source set. A class named in `mainClass` is then looked up under the source name and is
  not there.

- **Fix:** build the name from the same variable the relocation uses, so it cannot drift:

  ```groovy
  mainClass = relocatedPrefix + '.protocol.ProtocolVectorsCheck'
  ```

  Note this is per-module and per-loader: the two builds genuinely have different class names.

### A round-trip test cannot see two same-width fields swapped

- **Symptom:** a conformance check that decodes a message and re-encodes it, comparing bytes,
  reports success while one implementation writes `width` where the other expects `height`.

- **Check that proves it:** the permutation is its own inverse, so no byte comparison can see it.
  Confirm in one line:

  ```python
  struct.pack("<HH", 1920, 1080).hex()   # 80073804  -> reads width=1920 height=1080
  struct.pack("<HH", 1080, 1920).hex()   # 38048007  -> reads width=1080 height=1920
  # re-encoding either as (width, height) gives 80073804, so both look correct
  ```

- **Cause:** `width`/`height`, `x`/`y` and `min`/`max` are all adjacent fields of the same width.
  The decoder reads them into the wrong names and then writes them back in the order it now
  believes, so the bytes round-trip perfectly and the mistake is invisible. This is the reason
  Protobuf and ASN.1 carry field tags.

- **Fix:** pin values to field **names** as well as bytes, and resolve them by reflection:
  `expect=Config.width=1920,Config.height=1080` on the vector line. Deliberately injecting the swap
  into the generator is the check that the guard works — see the `config_h264_high` failures it
  produces. Nested records (`Rect`'s `w`/`h`) are still uncovered; PROTOCOL.md 2.4 says so, and
  they rely on the end-to-end tests.

### Two fields holding the same wire bit, so the header and the body disagree

- **Symptom:** a decoded `Frame` reports `flags=0` and `is_keyframe()` false on a frame that is
  plainly a keyframe, while the conformance test passes -- because the bytes do round-trip. Every
  keyframe arrives looking like a delta frame.

- **Check that proves it:** compare the header's flags against what the body implies. The test now
  asserts this, so a regression is a named failure:

  ```
  frame_keyframe_full: header flags 0x1 but the body implies 0
  ```

  To see the bug that check was added for, delete the `b.flags = headerFlags;` line in
  `relay/src/protocol.cpp` and rebuild.

- **Cause:** `Message` carried a `flags` member *and* `Frame` carried its own. `encode_message` wrote
  the header from the member and `parse_message` filled the body from the same byte, so the two
  always agreed with the wire and disagreed with each other. It only showed up where a consumer reads
  the body, which is the relay's partner and, later, the mod.

- **Fix:** one owner. `Message` has no `flags`; `body_flags(const Body&)` derives the header byte
  from the body that defines it, and `encode_message` writes that. This is the same shape as the Java
  side, where the keyframe bit lives only on the message class. A redundant field is not a
  convenience, it is a second thing to forget to update.

### A C++ test binds a reference into a temporary and reads freed memory

- **Symptom:** assertions about a value read out of a returned container fail with garbage, while the
  same value is provably correct in the code that stored it. `-Wdangling-reference` warns about it;
  `-Werror` is what turns the warning into a build failure.

- **Check that proves it:** the warning names the line, and the fix is to copy:

  ```cpp
  const auto& ep = relay.endpoints().front();   // endpoints() returns by value: `ep` dangles
  const auto ep = relay.endpoints().front();    // a copy of the small struct
  ```

  Lifetime extension only applies when a reference binds *directly* to the temporary, not to a
  reference that a member function of the temporary returned.

- **Cause:** easy to write because the struct is small and the compiler does not usually complain in
  a release build. It reads as a reference to a stable object because `front()` looks like it belongs
  to something longer-lived.

- **Fix:** copy small POD-ish structs out of returned containers, as the Java check does anyway
  (`expect=Config.width=...` compares by value).

### `add_dependencies` cannot name the target `add_custom_command(OUTPUT)` creates

- **Symptom:** `CMake Error at CMakeLists.txt:57 (add_dependencies): The dependency target
  "protocol_vectors" of target "vectors_test" does not exist.` -- and the error appears *after*
  "Configuring done", so it looks like the build system is fine and only generation failed.

- **Check that proves it:** `cmake -S . -B build-cpp` exits non-zero at the generate step even though
  the configure step reported success. The difference is the exit code; the tail of the log shows
  both messages.

- **Cause:** `add_custom_command(OUTPUT ...)` creates a *file* target, not a task. Task targets come
  from `add_custom_target`, `add_executable` and `add_library`.

- **Fix:** wrap the rule.

  ```cmake
  add_custom_command(OUTPUT "${VECTORS}" COMMAND ... DEPENDS ...)
  add_custom_target(protocol_vectors DEPENDS "${VECTORS}")
  ```

### The relay is not on the path the test thinks it is

- **Symptom:** none, yet. Recorded because the check is cheap and the assumption is easy to make:
  the relay is **not** a Minecraft module. The root `build.gradle` discovers modules by scanning
  `versions/<mc>/` for a directory holding both `fabric/` and `neoforge/`
  (`build.gradle:11-13`, and the same scan in `settings.gradle:30-36`), so anything under `relay/` is
  invisible to Gradle and anything under `dist/` is rejected by the release gate at
  `.github/workflows/release-mod.yml:185`, which only allows `*-<version>-(fabric|neoforge|universal).jar`.

- **Check that proves it:**

  ```bash
  ./gradlew :remote-worker-fabric:protocolCheck   # Java codec
  ctest --test-dir build-cpp -C RelWithDebInfo   # C++ codec and the relay e2e test
  ```

  Both run independently. A green Gradle build says nothing about the C++ side, and a green
  `ctest` says nothing about the mod.

### A CLI flag that is parsed, logged, and never used

- **Symptom:** `--port 45999` is accepted without complaint and the daemon reports
  `listening on 127.0.0.1:58200`. `--bind 0.0.0.0` reports an address it did not bind. Both were
  threaded into the options struct, printed, and then dropped on the floor: `listen()` took a
  hardcoded `INADDR_LOOPBACK` and its own port-0, so the option never reached the call that used it.

- **Check that proves it:** run it and read the number it prints back, rather than trusting the
  parse. A flag whose effect cannot be seen from the program's own output is a flag to distrust.

  ```bash
  ./build-cpp/bin/RelWithDebInfo/remote-worker-relay.exe --bind 127.0.0.1 --port 45999
  ./build-cpp/bin/RelWithDebInfo/remote-worker-relay.exe --bind 127.0.0.1 --port 45999   # again
  ```

  Both must report 45999. Before the fix both reported a random port. A second live bind of the same
  port must fail with "cannot bind" — note that `SO_REUSEADDR` lets a *just-exited* process rebind,
  so run the first one in the background to make the conflict real.

- **Cause:** options live in a struct, the parser fills it, and nothing checks that the consumer read
  it. Accepting a flag is not the same as honouring it, and `--help` makes the former look like the
  latter.

- **Fix:** pass the value through, and make the observable output reflect it. `listen()` now takes
  both address and port and reports the port the OS actually bound, so a mismatch between what was
  asked for and what was got is visible rather than silent.

### <symptom as the user or a log would describe it>

- **Symptom:**
- **Check that proves it:**
- **Cause:** `file:line`
- **Fix:**
