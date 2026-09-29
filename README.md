# Remote Worker

Control another computer from inside Minecraft.

Two pieces: a **mod** for Fabric and NeoForge, and a **native agent** that runs on the machine being
viewed. A controller holds a list of endpoints and connects to each agent directly — no relay, no
discovery, no port forwarding.

## Status

This is not yet usable end to end, and it is worth being precise about where it stops.

| | state |
|---|---|
| Wire format and both codecs | **done** — a Java codec in the mod and a C++ codec in the agent, both checked against byte vectors generated from one normative script |
| Agent: key minting and machine identity | **done** — `remote-worker keygen`, a 160-bit key per machine, and the `machine_id` derived from it |
| Agent: authentication | **done** — the key gates every session, compared in constant time, and the agent refuses to start without one |
| Agent: capture and encode | **not started** — the agent authenticates a controller and then **refuses the session** with `UnsupportedCapture`, rather than opening one that would never produce a frame |
| Tablet item | **done** — craftable, right-click opens the endpoint screen |
| Mod: connect and authenticate | **done** — connects to an agent, sends `HELLO` and `OPEN_SESSION`, and reports exactly what came back |
| Mod: video, input | **not started** — no frame rendering, and nothing sends input to the agent |
| Transport encryption | **not started** — see the warning under [The agent](#the-agent-c) |

So a release today gives you a **working agent** and a **mod that can find it, authenticate, and open
a session request** — and then the agent answers `UnsupportedCapture`, because the capture backend is
the missing half. You can see the whole handshake work, end to end, and watch it stop at the one
place that is not written yet.

The tablet item is registered, craftable, and opens a screen listing the machines configured in
`config/remote-worker/endpoints.json` with a Connect button each. It shows the agent's real answer
rather than a progress spinner that never resolves.

**A machine cannot view itself.** The agent refuses a controller on the same machine with
`SelfConnection`, and the mod refuses a loopback address before it opens a socket. The agent's check
is the one that holds — see [The agent](#the-agent-c) and `PROTOCOL.md` §3.

## Build

```bash
./gradlew build              # default Minecraft version (gradle.properties -> mc)
./gradlew -Pmc=1.21.1 build  # a specific Minecraft version
```

> Gradle 8.14 must run on JDK 17–23 (the example targets Java 21). If your default `java` is newer,
> point `JAVA_HOME` at a JDK 21 or set `org.gradle.java.home` in `gradle.properties`.

## The tablet

Right-click it to open the endpoint screen: the machines you have configured, a Connect button each,
and whatever the agent said when you last tried.

A machine is added by editing `config/remote-worker/endpoints.json`:

```json
[
  {
    "label": "the office box",
    "host": "192.168.1.50",
    "port": 47311,
    "key": "rw1_BKRQJ734CTGK8F30ACBGBHSWA533ANF8"
  }
]
```

The **machine id is not in the file, and does not need to be**: the mod derives it from the key with
the same function the agent uses, so an endpoint cannot be configured with an id that disagrees with
its own key. If you rotate a key, delete the endpoint and re-add it.

`host` must not be this machine. `localhost` and `127.x.x.x` are refused by the mod before it opens a
socket, and by the agent as `SelfConnection` — see below.

> The keys are stored in the clear in this file. Anyone who can read it can view every machine listed
> in it. It is the same deliberate simplification the agent makes with its own key file, and the
> piece most worth revisiting: this belongs in the OS credential store, which each loader exposes
> differently and neither has been done yet.

Adding an endpoint from inside the game is the obvious next step and is not built.

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

`ctest` runs three things: the same wire-format vectors the Java check uses against the C++ codec, a
credentials test, and an agent test that drives a real controller peer over loopback and asserts
what the agent accepts and refuses.

**What you will see today.** `keygen` prints a machine id and a key, and `agent` starts and listens.
Nothing can drive it yet, because the mod has no networking: a controller that connects today is
authenticated and then refused with `UnsupportedCapture`, and the agent logs why. That is the
intended behaviour rather than a stub — a controller left waiting on a screen that never arrives
cannot tell "not implemented" from "broken", so the agent says which it is.

**A controller holds a list of endpoints** and connects to each agent directly; there is no relay
and no discovery. An endpoint is configured, not found.

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

### Installing the agent

Every release carries one agent binary per platform, named
`remote-worker-<version>-<os>-<arch>`, under [Releases](#releases):

| Platform | Asset suffix |
| --- | --- |
| Windows x86-64 | `windows-x86_64.exe` |
| Windows ARM64 | `windows-arm64.exe` |
| Linux x86-64 | `linux-x86_64` |
| Linux ARM64 | `linux-arm64` |

Download the one matching your machine, mark it executable, and give it a key:

```bash
# Linux; use the right suffix for your architecture
curl -LO https://github.com/retiredroca/remote-worker/releases/download/v<version>/remote-worker-<version>-linux-x86_64
chmod +x remote-worker-<version>-linux-x86_64
./remote-worker-<version>-linux-x86_64 keygen
./remote-worker-<version>-linux-x86_64 agent
```

On Windows, download `remote-worker-<version>-windows-x86_64.exe` (or `windows-arm64.exe`) and run
it directly.

Verify what you downloaded, since there is no signature on any platform:

```bash
shasum -a 256 remote-worker-<version>-linux-x86_64
# compare against the asset digest GitHub shows on the release page
```

**macOS is not built, and is not planned.** It was dropped rather than supported badly: the only
runner available for it was arm64, and the x86_64 runner was being retired, so it would have meant a
platform with no build for the users most likely to want it. There is no `brew` formula and no
`.pkg`. The wire format still has a macOS value in the `os` field of a `HELLO` (`PROTOCOL.md`), since
that is what the format describes rather than what we build.

**A Linux agent is a real product decision, not just a build.** It has no capture backend yet, so it
can pair and be refused for that, and nothing more. Capture on an unattended Linux box needs
X11/Wayland and DRM in the picture, which is a different piece of work from producing a binary.

### Building the agent locally

The source compiles on all four targets: `stream.cpp`, `agent.cpp` and `credential.cpp` guard
their platform sections with `_WIN32`, `generate_token()` reads `/dev/urandom` off Windows, and the
key file defaults to `$HOME/.config/remote-worker/`.

**Those POSIX branches are now compiled and tested on every release**, which is the only reason they
are trustworthy. They were not always: the first CI run of the release workflow failed three times
in a row on `stream.cpp` (missing `<netdb.h>`), `credential.cpp` (`std::chmod` where libstdc++ has
only `::chmod`) and a `#` that had lost a slash in a scripted comment. A local MSVC build cannot see
any of that, because it never compiles a `#ifndef _WIN32` branch.

The `cmake` and `ctest` commands above are the same on both platforms, minus `--config`. Linux:

```bash
sudo apt install build-essential cmake   # or your distribution's equivalent
cmake -S . -B build-cpp -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-cpp
ctest --test-dir build-cpp --output-on-failure
./build-cpp/bin/remote-worker keygen
```

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
- builds and `ctest`s the agent natively on four GitHub-hosted runners — `ubuntu-latest` and
  `windows-latest` for x86_64, `ubuntu-24.04-arm` and `windows-11-arm` for arm64,

  requires all of it to pass, and creates the GitHub release with the jars and one agent binary per
platform. The private key never leaves your machine; CI only builds what the signed tag names, and
reads the version you already committed rather than re-stamping it.

**Every release must carry the jars and one agent binary per platform.** The workflow fails if any
is missing or misnamed, because a release with a jar-only set looks exactly like a successful one
and the omission is invisible until someone looks for the binary. A platform that fails to build or
fails `ctest` stops the whole release rather than shipping a partial set.

**This project does not publish to CurseForge or Modrinth.** That came from the template it was
generated from, along with three workflows and a set of `--curseforge` / `--modrinth` flags that
existed only to drive them; all of it has been removed. A release here means the GitHub release.

To publish a release that is not from the tag flow (e.g. a single-platform emergency build), the
classic local path still exists and still uploads the host binary — but it can only ever produce the
host platform's agent. Prefer `--ci`.

A release started from the Actions tab rebuilds an **existing** tag (`workflow_dispatch` with the
tag) without re-bumping anything. This is how a failed run is retried without a new version.

The staged build groups (`releaseLoaderJars`, `releaseLoaderBundles`, `releaseUniversal`,
`releaseUniversalBundles`) each write their own directory; `releaseJars` unions them into
`build/release/`, and the agent binary is staged *after* that, because `releaseJars` is a `Sync`
task and would delete it.

See `PROJECT-GUIDE.md` for building and the build layout.

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
│  ├─ src/main/java/.../RemoteWorker.java      # MOD_ID, id(), the platform accessor
│  ├─ src/main/java/.../item/          # the tablet, and the holder that resolves it
│  ├─ src/main/java/.../client/        # endpoints, the agent client, the tablet screen
│  ├─ src/main/java/.../credential/    # key format + machine id derivation (matches the C++)
│  ├─ src/main/java/.../platform/      # the seam each loader implements
│  └─ src/main/resources/              # model, lang, texture, and the crafting recipe
│  └─ src/main/java/.../protocol/       # wire-format codec, checked against the vectors
│  └─ src/test/java/.../protocol/       # the conformance check, run from `check`
├─ fabric/                              # Loom build; compiles common into <group>.fabric.common
└─ neoforge/                            # NeoGradle build; compiles common into <group>.neoforge.common
tools/{versioning,release,protocol_vectors}.py  # version scheme, local release driver, protocol encoder
tools/make_tablet_texture.py                    # regenerates the tablet texture
tools/check_posix_includes.py                  # catches a POSIX header a Windows build cannot see
```

The two loader builds are separate (Loom and NeoGradle cannot share one Gradle project). The shared
sources are compiled once per loader and merged into `<module>-<version>-universal.jar`, which works
on both loaders.

## Where to read more

| document | what is in it |
|---|---|
| `PROTOCOL.md` | the wire format, and why it is shaped the way it is — including why the key is the machine's identity and why it is not derived from a MAC or IP address |
| `RELEASE-GUIDE.md` | the release flow, end to end |
| `PROJECT-GUIDE.md` | building, adding a Minecraft version, the build layout, troubleshooting |

The mod's build layout — two loader builds, shared sources, a universal jar — is the multi-version
template's, and is described in `PROJECT-GUIDE.md`.
