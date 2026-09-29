#!/usr/bin/env python3
"""Local release driver.

The normal path is `--ci`: bump the version, commit, create a *signed* tag, push it, and stop. The
tag is the hand-off -- .github/workflows/release-ci.yml builds the mod jars and one native agent
binary per platform, tests them, and creates the GitHub release. The private key never leaves this
machine, and CI reads the version committed here rather than re-stamping it.

    python tools/release.py --mod all --ci             # the release path
    python tools/release.py --mod all --ci --dry-run

This project does not publish to CurseForge or Modrinth; that came from the template it was
generated from. The --curseforge / --modrinth / --no-ci-publish flags and the publishing code behind
them have been removed, along with the template workflows that existed only to call them.

The classic local path (without --ci) still builds and publishes a GitHub release from this machine,
which can only ever produce the *host* platform's agent binary. It is kept as a single-platform
fallback.

The tag is a single series `v<api 3 parts>.<stamp>` (e.g. `v1.0.2.26091512`).

Component ids come from the project's own modules (versions/<mc>/*/module.properties), so the
`--mod` choices and the bundle names are never hardcoded here.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

import versioning

ROOT = Path(__file__).resolve().parent.parent
VERSIONS = ROOT / "versions.properties"
DIST = ROOT / "dist"
UA = "mod-template-release"


def log(msg):
    print(f"release: {msg}")


def die(msg):
    sys.exit(f"release: error: {msg}")


# A release jar is <id>-<version>-<loader>.jar, matching jarFileName in gradle/versions.gradle.
# Matching the shape rather than a list of ids means a new module needs no edit here, and it rejects
# every pre-rename shape (a leading loader, or a dot before the version), which a count check cannot.
# The id group allows dashes so a `bundle-` prefix matches.
ALLOWED_JAR = re.compile(r"^[A-Za-z0-9_-]+-[0-9][0-9.]*-(fabric|neoforge|universal)\.jar$")

# The endpoint agent: the one thing that runs on a watched machine. Its version is the same stamped
# version as the mod, and the platform is named rather than implied, because a download page has to
# be able to tell a Windows user which file to take.
#
# Built and shipped by CI, on the --ci path, one binary per platform. This script only ever stages
# the host platform's, because a local build cannot produce a binary for a platform it is not
# running on. That is the reason the whole build moved to CI.
ALLOWED_BINARY = re.compile(
    r"^remote-worker-[0-9][0-9.]*-[a-z0-9]+-(x86_64|arm64)(\.exe)?$")

# The files in a release directory that are not jars. Deliberately a shape test rather than a
# match against ALLOWED_BINARY: the point of the check is to catch a stray file as well as a
# missing one, and filtering by the allowed shape would quietly ignore exactly the wrong name.
# .gitkeep and changelog.md are the directory's own bookkeeping, not release content.
def non_jar_files(directory):
    return sorted(
        p for p in Path(directory).iterdir()
        if p.is_file()
        and not p.name.endswith(".jar")
        and not p.name.startswith(".")
        and p.name != "changelog.md"
    )


# --- process helpers ----------------------------------------------------------------


def run(cmd, dry=False, **kw):
    printable = " ".join(str(c) for c in cmd)
    if dry:
        print(f"  [dry-run] {printable}")
        return subprocess.CompletedProcess(cmd, 0, b"", b"")
    log(printable)
    return subprocess.run(cmd, cwd=ROOT, check=True, **kw)


def capture(cmd):
    return subprocess.run(cmd, cwd=ROOT, check=True, capture_output=True, text=True).stdout


def gradlew():
    # On Windows run the wrapper through cmd.exe: a bare "bash <win-path>" resolves to WSL's bash
    # (or a bash that strips the backslashes) and fails with "No such file or directory".
    if os.name == "nt":
        bat = ROOT / "gradlew.bat"
        if bat.exists():
            return ["cmd", "/c", str(bat)]
    return [str(ROOT / "gradlew")]


# --- versions.properties ------------------------------------------------------------


def read_props(path):
    props = {}
    if not path.exists():
        return props
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, _, value = line.partition("=")
            props[key.strip()] = value.strip()
    return props


def set_prop(text, key, value):
    # [^\r\n]* rather than .* so a CRLF file gets a clean LF-terminated replacement.
    pattern = re.compile(rf"(?m)^{re.escape(key)}=[^\r\n]*")
    if not pattern.search(text):
        die(f"versions.properties has no '{key}=' line")
    return pattern.sub(f"{key}={value}", text)


def module_ids(mc):
    """Every module id under versions/<mc>/, in directory order."""
    return [props.get("id") for _, props in versioning.modules(mc) if props.get("id")]


def library_ids(mc):
    return [lib_id for lib_id, _, _ in versioning.libraries(mc)]


def bump(mc, mod, stamp, dry=False):
    """Re-version the named component(s). Only the ones named get a new version."""
    text = VERSIONS.read_text(encoding="utf-8")
    current = read_props(VERSIONS)
    ids = module_ids(mc)
    if mod == "all":
        targets = ids
    elif mod in ids:
        targets = [mod]
    else:
        die(f"unknown mod '{mod}' (known: {', '.join(ids)}, all)")
    changed = {name: False for name in ids}
    new_versions = dict(current)
    for comp in targets:
        new_versions[comp] = versioning.bump(current[comp], stamp)
        text = set_prop(text, comp, new_versions[comp])
        changed[comp] = True
    # newline="\n": without it Windows text mode writes CRLF, and CI then reads the version
    # values with a trailing \r (which breaks the publish file paths).
    # --dry-run must not touch the file: a later real run would then bump an already-bumped value
    # and produce a double stamp.
    if not dry:
        VERSIONS.write_text(text, encoding="utf-8", newline="\n")
    log(f"bumped {', '.join(targets)} to stamp {stamp}")
    return changed, new_versions


def bump_semantic(mc, mod, part, dry=False):
    """Move the semantic version line, dropping the stamp so the build re-stamps it.

    This is the only supported way to change <major>.<minor>.<patch>: the tooling stays the sole
    writer of versions.properties, so a hand edit cannot drift from what the build and the tag
    expect. A feature branch takes the next patch so its jars are distinguishable from the released
    line by filename; a release that needs more asks for minor or major explicitly.
    """
    text = VERSIONS.read_text(encoding="utf-8")
    current = read_props(VERSIONS)
    ids = module_ids(mc)
    if mod == "all":
        targets = ids
    elif mod in ids:
        targets = [mod]
    else:
        die(f"unknown mod '{mod}' (known: {', '.join(ids)}, all)")
    changed = {name: False for name in ids}
    new_versions = dict(current)
    for comp in targets:
        new_versions[comp] = versioning.bump_semantic(current[comp], part)
        text = set_prop(text, comp, new_versions[comp])
        changed[comp] = True
    if not dry:
        VERSIONS.write_text(text, encoding="utf-8", newline="\n")
    log(f"bumped {', '.join(targets)} to {part}: "
        + ", ".join(f"{c}={new_versions[c]}" for c in targets))
    return changed, new_versions


# --- build --------------------------------------------------------------------------


def build(mc, dry, no_daemon=False, stamp=None):
    g = gradlew()
    # Reuse a warm Gradle daemon across these invocations by default: each one would otherwise pay a
    # cold JVM start and reconfigure every included build. --no-daemon reproduces a CI-like cold
    # build; CI always passes it, where each run is a fresh container.
    daemon = ["--no-daemon"] if no_daemon else []
    # One stamp, computed once, by versioning.stamp() and written into versions.properties for the
    # component being released. Gradle would otherwise compute its own stamp at build time, and the
    # two can disagree in two ways: an hour boundary between this call and the build, and any
    # component still holding a bare "1.0.0" (a project generated from the template), which
    # stampVersion() appends a fresh stamp to at build time. Either way the jar names below are built
    # from the python value while the jars on disk carry Gradle's, so every upload path misses and
    # skips silently. Passing the stamp in makes the two agree by construction.
    pin = [f"-PversionStamp={stamp}"] if stamp else []
    # Bootstrap: library artifacts must be in repo/ before dependents compile, because dependents
    # require a version floor ([<floor>,<upper>)).
    for lib_id in library_ids(mc):
        for loader in ("fabric", "neoforge"):
            run(g + daemon + pin + ["-p", f"versions/{mc}/{lib_id}/{loader}",
                     "publishMavenJavaPublicationToRepoRepository"], dry=dry)
    # Staged build (loader jars -> loader bundles -> universal -> universal bundles) as separate
    # invocations, so a failure names the layer that broke. The last one unions the staged dirs
    # into build/release/ and republishes the libraries to ./repo.
    # --refresh-dependencies: the floor range was just republished, so don't use a cached resolution.
    steps = [
        ["releaseLoaderJars"],
        ["releaseLoaderBundles"],
        ["releaseUniversal"],
        ["releaseUniversalBundles", "releaseJars", "publishLibrary"],
    ]
    for step in steps:
        run(g + daemon + pin + ["--console=plain", f"-Pmc={mc}", "--refresh-dependencies"] + step, dry=dry)


def bundle_definitions():
    """bundles.properties as an ordered {name: [module ids]} map."""
    bundles_file = ROOT / "bundles.properties"
    if not bundles_file.exists():
        return {}
    props = read_props(bundles_file)
    out = {}
    for key in sorted(k for k in props if k.startswith("bundle.")):
        ids = [i.strip() for i in props[key].split(",") if i.strip()]
        if ids:
            out[key[len("bundle."):]] = ids
    return out


def expected_jars(mc):
    """How many jars the release set must contain: per-loader + universal + bundle variants.

    Each module contributes one jar per loader plus one universal jar; each bundle contributes a
    universal container plus one per-loader container, so three files per bundle.
    """
    ids = module_ids(mc)
    loaders = ["fabric", "neoforge"]
    loader_jars = len(ids) * len(loaders)
    universal = len(ids)
    bundles = len(bundle_definitions())
    return loader_jars + universal + bundles * (1 + len(loaders))


def collect(mc, dry):
    if not dry:
        # Clear stale artifacts, but keep the tracked dist/.gitkeep.
        DIST.mkdir(exist_ok=True)
        for path in DIST.iterdir():
            if path.name == ".gitkeep":
                continue
            if path.is_dir():
                shutil.rmtree(path)
            else:
                path.unlink()
        release = ROOT / "build" / "release"
        jars = sorted(release.glob("*.jar"))
        if not jars:
            die(f"no jars in {release}; did the build run?")
        for jar in jars:
            shutil.copy2(jar, DIST)
        # The binary is required, not optional. A release with no agent in it looks exactly like a
        # successful release -- the jar count is right, the build was green -- and the omission is
        # only visible to someone who then cannot find the file to install. So it is checked here
        # rather than discovered on a download page.
        binaries = non_jar_files(release)
        if not binaries:
            die(f"no agent binary in {release}; did the C++ build run? "
                f"(expected something like remote-worker-<version>-windows-x86_64.exe)")
        for binary in binaries:
            shutil.copy2(binary, DIST)
    verify(mc, dry)


def verify(mc, dry):
    if dry:
        return
    jars = sorted(p.name for p in DIST.glob("*.jar"))
    expected = expected_jars(mc)
    if len(jars) != expected:
        die(f"expected {expected} release jars, found {len(jars)}: {jars}")
    # The count alone cannot tell a correct jar from a stale one: a set of pre-rename jars has the
    # same length as the current set, so `build/release` leftovers from an earlier naming scheme
    # would be copied into dist/ and published. Check the shape too.
    bad = [j for j in jars if not ALLOWED_JAR.match(j)]
    if bad:
        die(f"unexpected files in dist/ (not <id>-<version>-<loader>.jar): {bad}")
    binaries = [p.name for p in non_jar_files(DIST)]
    if len(binaries) != 1:
        die(f"expected exactly 1 agent binary in dist/, found {len(binaries)}: {binaries}")
    if not ALLOWED_BINARY.match(binaries[0]):
        die(f"unexpected agent binary name (want remote-worker-<version>-<os>-<arch>[.exe]): "
            f"{binaries[0]}")
    log(f"release set OK ({len(jars)} jars + {binaries[0]})")


def changelog(tag, dry):
    if dry:
        return
    DIST.mkdir(exist_ok=True)
    try:
        prev = capture(["git", "describe", "--tags", "--abbrev=0", "HEAD^"]).strip()
    except subprocess.CalledProcessError:
        prev = ""
    if prev:
        log_range = [f"{prev}..HEAD", f"..HEAD"]
        header = f"Changes since {prev}"
    else:
        log_range = ["HEAD"]
        header = "Initial release"
    lines = [header, ""]
    for rng in log_range:
        out = capture(["git", "log", rng, "--no-merges", "--pretty=format:- %s (`%h`)"])
        if out.strip():
            lines.append(out.strip())
            break
    (DIST / "changelog.md").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    log("wrote dist/changelog.md")


# A bundle's version is not computed here. Gradle decides it (1.0.0.<newest stamp among the
# contents' *built* versions) and writes it into the filename; the only correct way to learn it is to
# read it back off the built jar. The two helpers that used to predict it from versions.properties
# plus a fresh versioning.stamp() were never called, and were wrong twice over: a second clock read
# could disagree with the one the build used, and a component still holding a bare "1.0.0" is stamped
# by Gradle at build time, which the prediction could not see. Read the filename instead, as
# mc-storage-area-network's tools/release.py bundle_versions() does.


# --- git ----------------------------------------------------------------------------


def repo_slug():
    try:
        url = capture(["git", "remote", "get-url", "origin"]).strip()
    except subprocess.CalledProcessError:
        url = ""
    match = re.search(r"github\.com[:/](.+?)(?:\.git)?$", url)
    return match.group(1) if match else ""


def current_branch():
    return capture(["git", "rev-parse", "--abbrev-ref", "HEAD"]).strip()


def ensure_clean(allow_dirty):
    if allow_dirty:
        return
    if capture(["git", "status", "--porcelain"]).strip():
        die("working tree is dirty; commit/stash first or pass --allow-dirty")


def commit(paths, message, dry, sign=True):
    # Only stage paths that exist: a project without a library module has no repo/ directory, and
    # `git add -- <missing>` aborts the whole release.
    existing = [str(p) for p in paths if Path(p).exists()]
    if not existing:
        log("nothing to stage; skipping commit")
        return False
    run(["git", "add", "--"] + existing, dry=dry)
    if not dry and capture(["git", "diff", "--cached", "--name-only"]).strip() == "":
        log("nothing staged; skipping commit")
        return False
    cmd = ["git"]
    if not sign:
        cmd += ["-c", "commit.gpgsign=false"]
    cmd += ["commit", "-m", message]
    run(cmd, dry=dry)
    return True


def tag_release(tag, message, sign, dry):
    """Create the release tag locally (signed unless --unsigned) so it carries a GPG signature."""
    if not dry and capture(["git", "tag", "--list", tag]).strip():
        die(f"tag {tag} already exists; delete it or release with --tag")
    cmd = ["git"]
    if sign:
        cmd += ["tag", "-s", tag, "-m", message]
    else:
        cmd += ["-c", "tag.gpgSign=false", "tag", tag, "-m", message]
    run(cmd, dry=dry)


# --- GitHub API ---------------------------------------------------------------------


def github_token():
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if token:
        return token
    out = subprocess.run(
        ["git", "credential", "fill"],
        input=b"protocol=https\nhost=github.com\n\n",
        capture_output=True,
    ).stdout.decode()
    for line in out.splitlines():
        if line.startswith("password="):
            return line[len("password="):]
    die("no GITHUB_TOKEN and no stored github.com credential")


def gh_request(method, url, token, data=None, content_type="application/vnd.github+json"):
    body = json.dumps(data).encode() if data is not None else None
    req = urllib.request.Request(url, data=body, method=method)
    req.add_header("Authorization", f"token {token}")
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", UA)
    req.add_header("Content-Type", content_type)
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            payload = resp.read().decode()
            return resp.status, (json.loads(payload) if payload else {})
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def github_release(token, tag, target, dry):
    if dry:
        print(f"  [dry-run] create GitHub release {tag} at {target}")
        return {"id": 0, "upload_url": ""}
    status, resp = gh_request("POST", f"https://api.github.com/repos/{repo_slug()}/releases", token, {
        "tag_name": tag,
        "target_commitish": target,
        "name": tag,
        "body": "",
        "draft": False,
        "prerelease": False,
    })
    if status not in (200, 201):
        die(f"GitHub release failed ({status}): {resp}")
    log(f"GitHub release {tag} created")
    return resp


def upload_assets(token, release_id, dry):
    assets = sorted(list(DIST.glob("*.jar")) + non_jar_files(DIST))
    for asset in assets:
        if dry:
            print(f"  [dry-run] upload {asset.name}")
            continue
        url = f"https://uploads.github.com/repos/{repo_slug()}/releases/{release_id}/assets?name={asset.name}"
        req = urllib.request.Request(url, data=asset.read_bytes(), method="POST")
        req.add_header("Authorization", f"token {token}")
        # Per asset: a jar is a jar, and an executable is not. Sending the wrong type does not fail
        # the upload, it just makes the release page describe the file wrongly.
        content_type = ("application/java-archive" if asset.name.endswith(".jar")
                        else "application/vnd.microsoft.portable-executable")
        req.add_header("Content-Type", content_type)
        req.add_header("User-Agent", UA)
        with urllib.request.urlopen(req, timeout=600) as resp:
            log(f"uploaded {asset.name} ({resp.status})")


# --- orchestration ------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(description="Build and publish a release locally.")
    ap.add_argument("--mc", default="1.21.1", help="Minecraft version folder under versions/")
    ap.add_argument("--mod", required=True,
                    help="which component is changing: a module id or 'all' (the tag is always "
                         "v<<library 3 parts>>.<stamp>)")
    ap.add_argument("--tag", default=None)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--unsigned", action="store_true", help="do not GPG-sign the commit and tag")
    ap.add_argument("--allow-dirty", action="store_true")
    ap.add_argument("--no-push", action="store_true")
    ap.add_argument("--skip-build", action="store_true", help="reuse the existing dist/")
    ap.add_argument("--no-daemon", action="store_true",
                    help="do not reuse a Gradle daemon (slower; reproduces a CI-like cold build)")
    ap.add_argument("--ci", action="store_true",
                    help="bump, commit, sign and push a tag, then stop: the release workflow "
                         "(.github/workflows/release-ci.yml) triggered by the tag builds the jars and "
                         "one native agent per platform, tests them, and creates the GitHub release. "
                         "This is the release path for this project, because a local build can only "
                         "ever produce the host platform's agent binary.")
    ap.add_argument("--local-only", action="store_true",
                    help="build and stage the jars locally only: no commit, tag, push, GitHub release "
                         "or platform/CI publishing. versions.properties is still bumped so the local "
                         "jars carry the next version; undo with `git checkout -- versions.properties`.")
    ap.add_argument("--bump", choices=("patch", "minor", "major"), default=None,
                    help="move the semantic version line and stop: no build, no tag, no push. A feature "
                         "branch takes the next patch so its jars are distinguishable from the released "
                         "line by filename. Left uncommitted unless --commit is also given.")
    ap.add_argument("--commit", action="store_true",
                    help="with --bump, also commit the change (otherwise it is left staged for review)")
    args = ap.parse_args()

    dry = args.dry_run
    mc = args.mc
    ensure_clean(args.allow_dirty)

    # --bump is a standalone, version-only operation: it writes versions.properties and stops. It
    # never builds or publishes, so it is safe to run in a worktree or a plain checkout.
    if args.bump:
        changed, versions = bump_semantic(mc, args.mod, args.bump, dry)
        if not dry and args.commit:
            commit([VERSIONS], f"Bump {args.mod} to the {args.bump} line", dry, not args.unsigned)
        elif not dry:
            log("left uncommitted; review with `git diff versions.properties`, then "
                "`git commit -am` or re-run with --commit")
        return

    slug = repo_slug()

    libs = library_ids(mc)
    ids = module_ids(mc)
    if not ids:
        die(f"no modules found under versions/{mc}/")
    # The tag series is keyed on the library's version line; a project without a library (the
    # single-mod template) uses its first module instead.
    primary = libs[0] if libs else ids[0]

    stamp = versioning.stamp()
    changed, versions = bump(mc, args.mod, stamp, dry)

    # Dependent modules resolve a library through a floor range computed from versions.properties
    # at configuration time (see gradle/versions.gradle), so a bump needs no extra rewrite here.

    tag = args.tag or versioning.tag(versions[primary], stamp)
    log(f"tag: {tag}")

    # In --ci mode nothing is built here: the tag is the hand-off, and the workflow triggered by
    # the tag builds the jars and one native agent per platform. Building locally would only ever
    # produce the host platform's binary, which is the whole reason the build moved to CI.
    if args.ci:
        pass  # deliberately no build, no collect, no verify
    elif not args.skip_build:
        build(mc, dry, args.no_daemon, stamp)
        collect(mc, dry)
    else:
        verify(mc, dry)
    if not args.ci:
        changelog(tag, dry)

    if args.local_only:
        log(f"local-only: built the {tag} jars into build/release/ and dist/")
        log("no commit, tag, push, GitHub release or platform publishing was performed")
        log("versions.properties was bumped; undo with: git checkout -- versions.properties")
        return

    sign = not args.unsigned

    # Commit the bump (so the tag points at the bumped versions), then push.
    bump_paths = [VERSIONS, ROOT / "repo"]
    if commit(bump_paths, f"Release {tag}: bump {args.mod} version", dry, sign):
        if not args.no_push and slug:
            run(["git", "push", "origin", current_branch()], dry=dry)
    tag_release(tag, f"Release {tag}", sign, dry)
    if not args.no_push and slug:
        run(["git", "push", "origin", tag], dry=dry)

    # --ci: the signed tag is pushed, and the workflow it triggers does everything else. Returning
    # here is what keeps the local script from building a host-only binary or creating a jar-only
    # GitHub release behind the workflow's back.
    if args.ci:
        if not slug:
            die("--ci needs a GitHub remote: the tag push is what triggers the release workflow")
        log(f"pushed the signed tag {tag}; .github/workflows/release-ci.yml builds and publishes it")
        log("  jars + one agent binary per platform are built and tested on CI, then released")
        log(f"  watch it here: https://github.com/{slug}/actions")
        return

    if not slug:
        log("no 'origin' GitHub remote; release is local only (committed + tagged, not pushed)")
        log("done")
        return

    # GitHub release against the tag we just pushed. This is the single-platform fallback path:
    # it can only carry the host platform's agent binary. The --ci path above is the real one,
    # because a release built here cannot produce a binary for a platform that is not this machine.
    token = github_token()
    target = capture(["git", "rev-parse", "HEAD"]).strip()
    release = github_release(token, tag, target, dry)
    if release:
        upload_assets(token, release["id"], dry)

    log("done")


if __name__ == "__main__":
    try:
        main()
    finally:
        # An idle Gradle daemon keeps Loom's cached mapping jars open, and the next build needs
        # exclusive access to re-merge them (FileSystemException: "being used by another process").
        # Stopping it here releases those handles; the next run pays one cold start instead.
        try:
            subprocess.run(gradlew() + ["--stop"], cwd=ROOT,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
        except Exception:
            pass
