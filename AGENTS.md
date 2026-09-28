# Agent working rules

General doctrine for this project. Project-specific facts — commands, paths, tool versions, machine
setup, and the traps that have already cost time — belong in [`GOTCHAS.md`](GOTCHAS.md). Add to it the
moment something bites; do not wait to be asked, and do not leave a conclusion without the check that
proves it.

Keep this file free of those specifics. Layout is in [`PROJECT-GUIDE.md`](PROJECT-GUIDE.md) and the
release workflow in [`RELEASE-GUIDE.md`](RELEASE-GUIDE.md) — reference them here rather than restating
them, so there is one copy of each to keep correct.

## Where project specifics go

- **Keep shared rules free of platform and project specifics.** Concrete commands, absolute paths, tool
  and version numbers, error strings and machine-local facts do not belong in this file. They go in this
  repository's `GOTCHAS.md`, referenced from here.
  A rule that only makes sense on one machine, or against one tool version, is a fact, not a rule.
- **When something costs you time, add a gotcha to `GOTCHAS.md` yourself — do not wait to be asked.**
  Write it at the moment it bites, not retrospectively. Include the symptom as
  observed, the check that proves it (a log, a file on disk, saved data, a byte comparison), the cause
  with `file:line`, and the fix. An entry that records only the conclusion is nearly worthless — the next
  reader cannot tell whether it still applies to their build.
- **Prefer the check over the conclusion.** Write down the command or observation that reproduces the
  state, so a later reader can verify rather than trust. "It stops after a while" is useless;
  "the dimension's `data/chunks.dat` only exists once the force ran, and its mtime against the
  overworld's `LastPlayed` shows whether it is still being saved" is not.
- **Scope and date every entry, and delete the ones that stop holding.** A stale gotcha is worse than no
  gotcha, because it gets trusted.
- **Reference, don't inline.** `AGENTS.md` points at `GOTCHAS.md` rather than restating it, so there is
  exactly one copy to keep correct.
- **Do not move a general rule down into a gotchas file.** If it holds for this whole project it stays
  here; if it holds only on this machine or in one module, it is a fact. Do not quietly promote a project fact to a shared rule
  either — that is the "Never generalize" failure in the other direction.

## How to work

- **Never assume — verify.** The whole codebase is here to read. Before claiming something is unused,
  dead, duplicated or broken, grep for it and read the code that actually runs. Do not assume an API,
  block, generator or value behaves as its name implies — read the actual source and the actual data.
  If it cannot be verified, say so and ask instead of guessing.
- **Confirm, don't assume.** Cite `file:line` for every claim about behaviour; say "unverified"
  otherwise. When the requirement is unclear, ask and confirm before coding — do not pick an
  interpretation and build on it.
- **When the maintainer conceptualizes, answer from how the system works — not from what you prefer.**
  They describe a goal, a behaviour or an idea rather than a precise instruction. Offer solutions
  derived from the facts of the underlying and connected systems: read the code, the vanilla/loader
  code it calls, and the data. Do not infer intent, and do not offer the option you would have chosen
  or the approach you would prefer. State what the code does, with `file:line`, and let the choice
  follow from that. Do not turn the answer into a discussion of your own reasoning or character.
- **A successful build is not proof of correctness.** It only proves the code compiles. Compiling on
  is exactly how a wrong-but-type-correct change ships; check the runtime semantics separately.
- **Never generalize.** Do not carry a conclusion from one class, mod or file over to its siblings,
  its Fabric/NeoForge twin, or the next thing that looks similar. Verify each one on its own.
- **Understand before you criticize.** Learn how a system works and *why* it is built that way before
  hunting for problems in it. Read the design docs and trace the actual data flow first; a finding
  that ignores intent is not a finding.
- **Prefer proof over plausibility when reporting:** cite `file:line` and show the check that backs it.

## Failure

- **Failing loudly is information, and it is cheap.** A loud failure tells you it failed, which a
  silent one never does. It teaches. Treat it as the fastest information available.
- **Never bypass a failure to make it pass — that *is* the failure.** Reaching for a workaround, a
  `try`/`catch`, a relaxed assertion or a parallel "safe" path to get green destroys the one fact
  worth having. Read the error and fix the real cause.
- **Being wrong is not the failure. Routing around being wrong is.** Say so plainly, correct the record,
  move on. No apology performance, no self-directed analysis, and never make the maintainer manage
  your feelings about your own behaviour.
- **One change at a time.** No speculative bundles. Each change maps to one confirmed cause and is
  built, deployed and tested before the next.
- **Implement the approved plan; never substitute your own.** Do not deviate for convenience, even if
  the alternative looks simpler. If the approved approach is wrong or blocked, STOP and say so. Disclose
  any shortcut or plan change and get agreement before building.
- **If a symptom contradicts the plan, suspect your own deviation first.**

## Method

- **Read all related code first** — yours, the vanilla/loader code it calls, and the data/config — end
  to end, before proposing anything.
- **Fix causes, not symptoms.** Get evidence (log, trace, saved data) before editing; fix the code that
  produces the behaviour, not the thing that displays it.
- **Report the diagnosis before proposing a fix.** What the code does (cited), what happens, where they
  diverge, ranked causes, the smallest change, and unknowns called out.
- **Host faithfully.** Parts the mod hosts are real blocks/objects and must behave as they do in the
  world; the fault is in how the mod hosts them. Do not ask the maintainer to explain standard
  behaviour.
- **Keep the README current** for any user-facing change: behaviour, controls, build steps, layout,
  supported versions.

## Editing files

- **Line endings are mixed in this repo and `.gitattributes` sets `* -text`, so git does no
  normalization.** A stray carriage return therefore shows up as a whole-file rewrite, and the cause is
  usually the editor rather than the content: text modes, convenience rewrites and scripted edits each
  differ by platform, and several convert silently. After editing, compare each touched file's line
  endings against `HEAD` and restore the original style. Verify with a byte count rather than by eye —
  a diff that reads as one added line is actually a whole file changed.
- Prefer the edit tools over scripted rewrites for anything beyond a one-line change.
- Write ASCII by default. Keep to the file's existing tone and heading style.

## Git

- **Never commit, amend, tag, push, or GPG-sign automatically.** Finish the work, then tell the user it
  is ready and wait for an explicit instruction before creating any commit or tag.
- When notifying, report the working-tree status and the commit message you would use.
- Commits and tags are cryptographically signed with the maintainer's key. The signing agent caches the
  passphrase, and that cache expires: a signing attempt then fails waiting for input that never arrives.
  Retry once the user has unlocked it — do not re-sign, or bypass signing, to get past the failure.
- Never change git config, skip hooks, force-push, or create empty commits unless asked.

## Build & verify

These are the transferable rules. The concrete commands, paths, tool versions and error strings for a
given project belong in that project's own notes — they go stale and they are not what generalizes.

- **Run the toolchain on a runtime it actually supports.** The plugin versions these builds pin often
  predate the newest runtime, so a machine default that is too new fails during configuration with a
  class-file-version error naming a major version the toolchain does not recognise. Point the build at
  a supported runtime through the environment variable the build tool reads, and keep the exact path in
  the project notes rather than here — it is a property of the machine, not of the rule. Set it where
  the build tool will see it: the environment for a command line, the tool's own runtime setting for an
  IDE.
- **Select the target version through the property the build exposes**, not by hand-editing generated
  files. Keep the property name in the project notes; the rule is that the selection is an input, never
  a source edit.
- **Build in stages, then the union.** Run the per-target groups first, then the aggregate task, and
  check that the output directory holds the complete set afterwards. Groups that are legitimately empty
  for a given project should still be invoked so the sequence stays uniform across repositories.
- **Publish the shared module before dependents compile.** Dependents resolve a version floor from the
  local repository, so a changed interface symbol has to be published and the dependents refreshed
  before they will even compile against it. Publishing later produces artifacts that look built and
  are internally mismatched.
- **Deploy deliberately, then verify what landed.** Clear stale artifacts before copying so an old build
  cannot masquerade as a new one, and afterwards confirm the deployed file is byte-identical to what was
  just built. A stale deploy reproduces "fixed nothing" and costs an entire diagnostic sweep.
- **JVM settings are per build root, not per project.** Settings are read from the nearest properties
  file, so every nested build resolves its own. Repeating the same values in each nested root is
  deliberate, not duplication to clean up; if they drift, the build silently starts a second process
  with different limits and the symptom looks like slowness rather than misconfiguration. Do not raise
  the shared heap to fix a per-worker out-of-memory error — each parallel worker is its own process, so
  the worker count is the lever, kept below the core count.
- **Reuse a warm build process locally; force a clean one in CI.** Asking the build tool for a fresh
  process re-initialises and re-configures every included build, which dominates wall-clock on a large
  composite. Continuous integration should force the clean path because it gets a fresh environment
  anyway; a local release script should not.
- **Treat the incremental-build cache as off unless measured.** It can be unreliable against the plugin
  versions these builds pin, and a cache that reports success from a stale input is worse than no cache
  at all.
- **Version bumps are declarative:** only the component explicitly named is re-versioned, and nothing
  infers the target from which files were touched. When one change spans the shared module and its
  dependents, release them together so the artifacts cannot mix new and old component versions.

## Conventions

- **Anything shared across gameplay mods belongs in the shared module** (`api` / `library`), never
  duplicated per loader or per mod.
- Follow the per-loader layout: `common` + `fabric` + `neoforge` included builds, shared sources
  relocated per loader so one universal jar holds both mapping variants, and a loader-agnostic platform
  seam installed by each loader initializer.
- Mixins are per loader and delegate to shared logic in `common`; the per-loader class supplies only the
  injection point. Keep mixin configs at `defaultRequire: 1` — let a failed injection fail loudly.
- Keep versions in a single scheme shared across the repositories, with the timestamp component
  generated by the project's own versioning helper rather than typed by hand, and tags that mirror the
  version they mark.
