# Releasing firmware to GitHub

How to turn on automatic GitHub releases and how to publish a new firmware
version. You only do the **one-time setup** once; after that, publishing a
release is just "bump the version, commit, push, build."

Firmware is always built **locally** in PlatformIO — GitHub never builds it. The
release step only *uploads* the binary you built and creates the release entry.

---

## One-time setup (do this once, when you want releases turned on)

Until you finish this, nothing changes: every build just copies the firmware to
`dist/` as usual and the release step is silently skipped. No GitHub account is
needed for normal building, USB flashing, or dashboard OTA.

### 1. Install the GitHub CLI (`gh`)

In a terminal (PowerShell):

```powershell
winget install GitHub.cli
```

(or download from <https://cli.github.com/>). **Open a new terminal** afterward
so `gh` is on your PATH, then check:

```powershell
gh --version
```

### 2. Log in

```powershell
gh auth login
```

Answer the prompts:

| Prompt | Choose |
| --- | --- |
| What account do you want to log into? | **GitHub.com** |
| Preferred protocol for Git operations? | **HTTPS** (matches this repo's remote) |
| Authenticate Git with your GitHub credentials? | **Yes** |
| How would you like to authenticate? | **Login with a web browser** |

It prints a one-time code (like `ABCD-1234`) and opens your browser. Paste the
code, click **Authorize**, and return to the terminal — it should say
`✓ Logged in as angeeinstein`. The credential is stored in the Windows
Credential Manager; **no token is ever put in the repo**.

### 3. Verify it's wired to the right repo

From the project folder:

```powershell
gh auth status          # should say: Logged in to github.com account angeeinstein
gh repo view            # should show: angeeinstein/bluetooth-scanner
gh release list         # should print nothing yet (no releases) — confirms read access
```

If `gh repo view` shows `angeeinstein/bluetooth-scanner`, you're done — `gh` resolves
the repo from the git remote, so the local folder name is irrelevant.

### Do I need to enable "Releases" on the repo first?

**No.** Releases are built into every GitHub repo — there's no feature to switch
on and no page to configure. The first `gh release create` just creates the
first release. Because it's your own repo, you already have the write access
that release creation needs (the default browser login grants the `repo` scope,
which covers releases). There is **no** manual repo-side setup.

---

## Publishing a new version

Day-to-day you rebuild and OTA-flash freely at the same version and nothing is
published. To cut a release, e.g. `1.0.1`:

1. **Changelog** — in [`CHANGELOG.md`](CHANGELOG.md), move your notes from
   `## [Unreleased]` into a new dated section: `## [1.0.1] - 2026-09-15`.
2. **Version** — set `FIRMWARE_VERSION` to `"1.0.1"` in
   [`src/version.h`](src/version.h) (same number).
3. **Commit and push.** This is required: the release tag points at the pushed
   commit, and the guard demands a clean tree so the published `.bin` provably
   matches committed source.
4. **Publish** — run the **`release`** task: in a terminal
   `…\platformio.exe run -t release`, or in the PlatformIO sidebar under *Project
   Tasks → esp32-s3-selftest → Custom → Publish release*.

That runs `gh release create v1.0.1 …`, attaches `dist/btscanner-v1.0.1.bin`,
and uses your `[1.0.1]` changelog section as the notes. If any guard isn't met it
tells you exactly why (e.g. "commit isn't pushed") and does nothing else.

> **Why a dedicated task and not just "build"?** A release *also* fires
> automatically after any build that actually recompiles `firmware.bin` — but a
> **no-op build** (nothing changed since the last build, e.g. you only committed)
> doesn't recompile, so the auto-release won't run. `run -t release` always runs
> the check regardless, so it's the reliable way to publish. When in doubt, use it.

### When publishing will *not* happen (all intentional)

- **Uncommitted changes** (dirty tree) → skip. This is the normal state while you
  iterate — build and OTA-flash as much as you want; nothing publishes.
- **Committed but not pushed** → prints "commit isn't pushed" and skips. Push,
  then run `release` again.
- **Version already released** → skip. Published versions are **immutable**; to
  ship a fix, bump to the next patch number instead of replacing an asset.
- **`gh` not installed / not logged in** → skip (with a hint from `run -t release`).
- **A plain build where nothing recompiled** → the auto-release doesn't fire (no
  recompile = the post-build step didn't run). Use `run -t release` to publish.

The decision is recomputed fresh every time from the current git state, so you
can bump the version, do many dirty builds, and only commit + push + publish
later — it publishes cleanly at that point using the built binary in `dist/`.

> **Heads-up on your first release:** the current version is `1.0.0` and has no
> release yet, so the first time you `run -t release` (or a build meets every
> condition above) it will publish **v1.0.0**. If you'd rather your first public
> release be a different number, bump `FIRMWARE_VERSION` first.

---

## Fixing mistakes

Made a release you didn't mean to? Delete it (and its tag):

```powershell
gh release delete v1.0.1 --cleanup-tag --yes
```

If your `gh` version doesn't support `--cleanup-tag`, delete the tag separately:

```powershell
gh release delete v1.0.1 --yes
git push origin :refs/tags/v1.0.1
```

---

## Troubleshooting: "I published but no release appeared"

First, always publish with `run -t release` — it prints the exact reason it
didn't publish, which usually answers this on its own. Then check each condition:

| Check | Command | Expected |
| --- | --- | --- |
| `gh` installed | `gh --version` | prints a version |
| Logged in | `gh auth status` | logged in to `angeeinstein` |
| Tree clean | `git status` | "nothing to commit, working tree clean" |
| Pushed | `git status` | "Your branch is up to date with 'origin/main'" |
| Version is new | `gh release view v1.0.1` | "release not found" (if it *exists*, it won't re-publish) |

Success prints `pkg_firmware: published GitHub release v…`. Any other outcome
prints a one-line reason (`working tree has uncommitted changes`, `commit isn't
pushed`, `already has a release`, `gh not found`, or a `not published -- …` tail
from `gh`) — fix that and run `release` again.

> If you relied on the **automatic** (build-triggered) release and nothing
> happened, the most common cause is a **no-op build**: nothing recompiled, so the
> post-build step didn't run. That's not a failure — just run `run -t release`.
