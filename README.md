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

## Relay and agent (C++)

The endpoint agent and the relay are C++20 under `relay/`, built with CMake and deliberately outside
`versions/` so the Gradle composite never sees them.

```bash
cmake -S . -B build-cpp
cmake --build build-cpp --config RelWithDebInfo
ctest --test-dir build-cpp -C RelWithDebInfo --output-on-failure
./build-cpp/bin/RelWithDebInfo/remote-worker-relay.exe --bind 127.0.0.1
```

`ctest` runs three things: the same wire-format vectors the Java check uses against the C++ codec, a
credentials test, and an end-to-end test that pairs an agent and a controller through the relay
over real loopback sockets and asserts the frames that cross are byte-identical.

The relay **refuses to bind a non-loopback address**. There is no transport encryption: anything
watching the wire can read the frames. `--allow-unauthenticated-lan` is the explicit decision point
to accept that on a given network, and the daemon warns while it is in effect.

**Endpoints are protected by a token the agent mints.** The agent generates it, shows it once, and
is the only party that checks it — the relay forwards it unread, so it holds no key and revocation
is per endpoint. The mod stores it in the OS keychain. `PROTOCOL.md` §3 has the flow, and why a
rejected token is reported to the controller as "no such endpoint": otherwise the relay becomes an
enumeration oracle for guessing which machine names are real.

## Protocol

`PROTOCOL.md` describes the control format between the mod, the endpoint agent, and the relay.
`tools/protocol_vectors.py` is the **normative encoder** -- the document is the prose, the script is
the definition -- and it generates byte vectors that every implementation is checked against:

```bash
python tools/protocol_vectors.py                        # writes build/protocol/vectors.txt
./gradlew :remote-worker-fabric:protocolCheck            # Java codec; also in ./gradlew build
ctest --test-dir build-cpp -C RelWithDebInfo            # C++ codec, same vectors
```

A field width that differs between the Java codec and the C++ relay or agent fails the build instead
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
the project was created (`TIMEZONE` in `tools/versioning.py`). One command drives a release:

```bash
python tools/release.py --mod all --dry-run   # preview
python tools/release.py --mod all             # build + GitHub release, CI publishes
```

The staged build groups (`releaseLoaderJars`, `releaseLoaderBundles`, `releaseUniversal`,
`releaseUniversalBundles`) each write their own directory; `releaseJars` unions them into
`build/release/`. Release workflows live in `.github/workflows/`; they create the GitHub release and
publish to CurseForge/Modrinth when the repository is configured for it (see `PROJECT-GUIDE.md` →
**Enabling CI publishing**).

See `PROJECT-GUIDE.md` for building, adding Minecraft versions and the build layout,
`RELEASE-GUIDE.md` for the release workflow, and `AGENTS.md` for repository conventions.

## Layout

```
PROTOCOL.md                                # the wire format (prose; the script below is the definition)
CMakeLists.txt                             # the C++ build root: relay/ and, later, agent/
relay/                                     # C++20 relay: codec, framing, routing
├─ include/rw/                             # wire.hpp protocol.hpp stream.hpp relay.hpp
├─ src/                                    # implementations + the daemon entry point
└─ tests/                                  # vectors_test.cpp, relay_e2e_test.cpp
versions/<mc>/version.properties        # Minecraft / loader / toolchain pins (canonical)
versions/<mc>/<module>/
├─ module.properties                    # id / name / group / authors / license / description
├─ common/                              # shared sources (no loader imports)
│  └─ src/main/java/.../protocol/       # wire-format codec, checked against the vectors
│  └─ src/test/java/.../protocol/       # the conformance check, run from `check`
├─ fabric/                              # Loom build; compiles common into <group>.fabric.common
└─ neoforge/                            # NeoGradle build; compiles common into <group>.neoforge.common
tools/{versioning,release,protocol_vectors}.py  # version scheme, local release driver, protocol encoder
```

The two loader builds are separate (Loom and NeoGradle cannot share one Gradle project). The shared
sources are compiled once per loader and merged into `<module>-<version>-universal.jar`, which works
on both loaders.

See the template's top-level README for the full workflow.
