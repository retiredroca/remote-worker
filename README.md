# Remote Worker

A Minecraft mod for multiple Minecraft versions on **Fabric** and **NeoForge**, built from the
multi-version template.

## Build

```bash
./gradlew build              # default Minecraft version (gradle.properties -> mc)
./gradlew -Pmc=1.21.1 build  # a specific Minecraft version
```

> Gradle 8.14 must run on JDK 17–23 (the example targets Java 21). If your default `java` is newer,
> point `JAVA_HOME` at a JDK 21 or set `org.gradle.java.home` in `gradle.properties`.

## The agent (C++)

The endpoint agent is C++20 under `agent/`, built with CMake and deliberately outside `versions/` so
the Gradle composite never sees it. It builds as one binary with subcommands:

```bash
cmake -S . -B build-cpp
cmake --build build-cpp --config RelWithDebInfo
ctest --test-dir build-cpp -C RelWithDebInfo --output-on-failure

# mint this machine's key, and print the machine id derived from it
./build-cpp/bin/RelWithDebInfo/remote-worker keygen

# serve one controller. A key is required: it will not start without one.
./build-cpp/bin/RelWithDebInfo/remote-worker agent
```

`ctest` runs four things: the same wire-format vectors the Java check uses against the C++ codec, a
credentials test, and an agent test that drives a real controller peer over loopback and asserts
what the agent accepts and refuses.

**The agent is the whole product.** A controller holds a list of endpoints and connects to each agent
directly; there is no relay and no discovery — an endpoint is configured, not found.

**There is no transport encryption.** Anything watching the wire can read the frames, and the agent
has no transport authentication, so it announces its machine id to anything that connects to the
port. It refuses everything until it sees a valid key, but that is a session gate, not a port gate.
`PROTOCOL.md` §3 has the details and what changes when TLS lands at the `ByteStream` seam.

**Each machine's identity is its key.** `keygen` mints a 160-bit key, prints it **once** alongside
the `machine_id` derived from it, and stores it owner-only. The id is what peers address the machine
by; a separate display label is never used for lookup. `--rotate` replaces the key, which necessarily
changes the machine id, so every controller has to be paired again — that is the point of rotating
one. The key is never derived from a MAC or IP address: those are public, so a credential computed
from one would be readable by anything on the wire, and duplicable across cloned VMs. `PROTOCOL.md` §3
has the reasoning in full.

### Building for Linux and macOS

> **Unverified.** Everything below is written from the source's own platform guards, not from a
> build that has happened. No Linux or macOS binary has been produced or tested, and no release has
> ever contained one. Treat the commands as a starting point to try, not as instructions known to
> work.

The source is written to compile on both: `stream.cpp`, `agent.cpp` and
`credential.cpp` guard their platform sections with `_WIN32`, `generate_token()` reads
`/dev/urandom` off Windows, and the key file defaults to `$HOME/.config/remote-worker/`. Those
`#else` branches are unexercised, and the POSIX socket code is the most likely thing to need
fixing.

What has and has not been checked: every source file compiles clean under GCC with
`-std=c++20 -Wall -Wextra -Wpedantic`, so the code is not MSVC-only. That is a **MinGW** build
(`x86_64-w64-mingw32`), which means it exercised the `_WIN32` branches and left every `#else` branch
untouched — it is evidence about the C++, not about POSIX. The socket layer in particular has never
been compiled where those branches are live.

The `cmake` and `ctest` commands above are the same on both platforms, minus `--config`. Linux:

```bash
sudo apt install build-essential cmake   # or your distribution's equivalent
cmake -S . -B build-cpp -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-cpp
ctest --test-dir build-cpp --output-on-failure
./build-cpp/bin/remote-worker keygen
```

macOS:

```bash
xcode-select --install              # if the command line tools are not already there
brew install cmake                  # Apple's CMake is stale for C++20
cmake -S . -B build-cpp -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-cpp
ctest --test-dir build-cpp --output-on-failure
./build-cpp/bin/remote-worker keygen
```

`ctest` passing on either platform is the real check, and the wire-format vectors and agent tests
are the parts most worth trusting there, since they are platform-independent by design.

Two things to expect beyond compilation:

- **macOS will not run an unsigned build.** Gatekeeper refuses a binary that was not signed and
  notarised, and the error is "cannot be opened because the developer cannot be verified". There is
  no workaround that is also a release path: it needs a Developer ID certificate, and notarisation
  by Apple. `xattr -cr remote-worker` clears the quarantine flag for a local test, and is not
  something to put in a download's instructions.
- **A Linux agent is a real product decision, not just a build.** It has no capture backend yet, so
  it can pair and be refused for that, and nothing more. Capture on an unattended Linux box needs
  X11/Wayland and DRM in the picture, which is a different piece of work from producing a binary.

## Protocol

`PROTOCOL.md` describes the control format between the mod and the endpoint agent.
`tools/protocol_vectors.py` is the **normative encoder** -- the document is the prose, the script is
the definition -- and it generates byte vectors that every implementation is checked against:

```bash
python tools/protocol_vectors.py                        # writes build/protocol/vectors.txt
./gradlew :remote-worker-fabric:protocolCheck            # Java codec; also in ./gradlew build
ctest --test-dir build-cpp -C RelWithDebInfo            # C++ codec, same vectors
```

A field width that differs between the Java codec and the C++ agent fails the build instead
of becoming subtly wrong bytes on the wire. Editing the format means editing the script and
`PROTOCOL.md` together.

## Adding a Minecraft version

```bash
./gradlew newVersion -Pnew=<mc>          # copies versions/1.21.1 and resolves its pins
./gradlew -Pmc=<mc> build
./gradlew portChangedFiles -Pfrom=1.21.1 -Pto=<mc>
```

## Releases

Versions use `<major>.<minor>.<patch>.<yymmddhh>`, with the stamp taken in the timezone chosen when
the project was created (`TIMEZONE` in `tools/versioning.py`).

This project does **not** release the way its sibling template projects do, because it also ships
`remote-worker`, a native C++ agent. A native binary is per-platform, and a single machine can only
build its own. So the whole build runs in CI, triggered by a tag you sign locally:

```bash
python tools/release.py --mod all --ci            # bump, commit, signed tag, push. CI takes over.
```

That is the whole local step. It bumps `versions.properties`, commits, creates a **signed** tag
(`git tag -s`), and pushes it. The tag push is the hand-off: `.github/workflows/release-ci.yml` then

- builds the mod jars on ubuntu (Java only, via `-PskipCpp=true`), and
- builds and `ctest`s the agent natively on `ubuntu-latest`, `windows-latest` and `macos-14`
  (linux-x86_64, windows-x86_64, macos-arm64),

requires all of it to pass, and creates the GitHub release with the jars and one agent binary per
platform. The private key never leaves your machine; CI only builds what the signed tag names, and
reads the version you already committed rather than re-stamping it.

**Every release must carry the jars and one agent binary per platform.** The workflow fails if any
is missing or misnamed, because a release with a jar-only set looks exactly like a successful one
and the omission is invisible until someone looks for the binary. A platform that fails to build or
fails `ctest` stops the whole release rather than shipping a partial set.

To publish a release that is not from the tag flow (e.g. a single-platform emergency build), the
classic local path still exists and still uploads the host binary — but it can only ever produce the
host platform's agent. Prefer `--ci`.

A release started from the Actions tab rebuilds an **existing** tag (`workflow_dispatch` with the
tag) without re-bumping anything.

> **The CI workflow has not run yet.** It was written against the code and tested where it can be
> (the release-set verification logic, and `-PskipCpp=true`, both exercised locally), but the
> Actions run itself is unverified. The first `--ci` release is the real test; watch the run and
> expect to iterate on it.

The staged build groups (`releaseLoaderJars`, `releaseLoaderBundles`, `releaseUniversal`,
`releaseUniversalBundles`) each write their own directory; `releaseJars` unions them into
`build/release/`, and the agent binary is staged *after* that, because `releaseJars` is a `Sync`
task and would delete it.

See `PROJECT-GUIDE.md` for building and the build layout, `RELEASE-GUIDE.md` for the template's
local-release flow, and `AGENTS.md` for repository conventions.

## Layout

```
PROTOCOL.md                                # the wire format (prose; the script below is the definition)
CMakeLists.txt                             # the C++ build root
agent/                                     # C++20 agent: codec, framing, sessions
├─ include/rw/                             # wire.hpp protocol.hpp stream.hpp credential.hpp agent.hpp
├─ src/                                    # implementations + main.cpp (agent, keygen)
└─ tests/                                  # vectors_test, credential_test, agent_test
versions/<mc>/version.properties        # Minecraft / loader / toolchain pins (canonical)
versions/<mc>/<module>/
├─ module.properties                    # id / name / group / authors / license / description
├─ common/                              # shared sources (no loader imports)
│  └─ src/main/java/.../protocol/       # wire-format codec, checked against the vectors
│  └─ src/test/java/.../protocol/       # the conformance check, run from `check`
├─ fabric/                              # Loom build; compiles common into <group>.fabric.common
└─ neoforge/                            # NeoGradle build; compiles common into <group>.neoforge.common
tools/{versioning,release,protocol_vectors}.py  # version scheme, local release driver, protocol encoder
tools/make_tablet_texture.py                    # regenerates the item texture, with an ASCII preview
```

The two loader builds are separate (Loom and NeoGradle cannot share one Gradle project). The shared
sources are compiled once per loader and merged into `<module>-<version>-universal.jar`, which works
on both loaders.

See the template's top-level README for the full workflow.
