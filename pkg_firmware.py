# PlatformIO post-build helper. Two additive jobs, run after firmware.bin is
# built. Neither changes how `pio run` / `pio run -t upload` (USB) behave.
#
#   1. Always: copy firmware.bin to ./dist/vibrashock-v<version>.bin (the
#      ready-to-upload OTA image; version from FIRMWARE_VERSION in version.h).
#
#   2. When the version is new: publish a GitHub release with that .bin attached
#      and notes from CHANGELOG.md. This only fires when ALL of these hold, so
#      ordinary local iteration never publishes:
#        - the `gh` CLI is installed and authenticated (`gh auth login`);
#        - the git working tree is clean (the built .bin matches committed code);
#        - the current commit is pushed (the release tag points at a remote commit);
#        - no release exists for this version yet.
#      Every check fails gracefully/silently, so a build never breaks over it.
#
# Two ways the release step runs:
#   - Automatically, after any build that actually (re)compiles firmware.bin.
#     A no-op build (nothing changed) does NOT recompile, so it won't fire.
#   - On demand: `pio run -t release` -- always runs the check regardless of
#     whether anything recompiled, and explains why it did/didn't publish. This
#     is the reliable way to publish; use it if an auto-release didn't fire.
#
# Wired up via `extra_scripts = post:pkg_firmware.py`. Remove that line (and this
# file) to disable. See RELEASING.md.

Import("env")  # noqa: F821  (PlatformIO SCons environment)

import os
import re
import shutil
import subprocess
import tempfile


def _run(cmd, timeout=25):
    """Run a command; return (returncode, combined_output). Never raises/hangs."""
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except Exception as e:
        return 1, str(e)


def _say(verbose, msg):
    if verbose:
        print("pkg_firmware: " + msg)


def _firmware_version():
    try:
        with open(os.path.join("src", "version.h"), encoding="utf-8") as f:
            m = re.search(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"', f.read())
            if m:
                return m.group(1)
    except Exception:
        pass
    return None


def _changelog_section(version):
    """Return the CHANGELOG.md body under '## [<version>]', or None."""
    try:
        with open("CHANGELOG.md", encoding="utf-8") as f:
            lines = f.read().splitlines()
    except Exception:
        return None
    start = None
    for i, ln in enumerate(lines):
        if re.match(r"^##\s*\[" + re.escape(version) + r"\]", ln):
            start = i + 1
            break
    if start is None:
        return None
    body = []
    for ln in lines[start:]:
        if re.match(r"^##\s*\[", ln):
            break
        body.append(ln)
    body = "\n".join(body).strip()
    return body or None


def _maybe_release(version, bin_path, verbose=False):
    """Publish a GitHub release for `version` if every guard passes.

    `verbose` (the `pio run -t release` path) explains each skip; the automatic
    post-build path stays quiet on the expected/common skips and only speaks up
    for the actionable near-miss (unpushed commit) and the final outcome.
    """
    if not version:
        return
    tag = "v" + version

    if shutil.which("gh") is None:
        _say(verbose, "gh CLI not found -- install it and run `gh auth login` to enable releases.")
        return
    if _run(["gh", "auth", "status"])[0] != 0:
        _say(verbose, "gh is not authenticated -- run `gh auth login`.")
        return

    rc, out = _run(["git", "status", "--porcelain"])
    if rc != 0:
        _say(verbose, "not a git repository -- can't publish a release.")
        return
    if out.strip():
        _say(verbose, "working tree has uncommitted changes -- commit them, then publish.")
        return

    if _run(["gh", "release", "view", tag])[0] == 0:
        _say(verbose, "%s already has a release (versions are immutable -- bump the version for a fix)." % tag)
        return

    sha = _run(["git", "rev-parse", "HEAD"])[1].strip()
    if not sha or not _run(["git", "branch", "-r", "--contains", sha])[1].strip():
        print("pkg_firmware: %s is a new version but its commit isn't pushed -- push it, then publish." % tag)
        return

    # Publish: create the release at this commit with the .bin attached.
    notes = _changelog_section(version)
    cmd = ["gh", "release", "create", tag, bin_path, "--title", tag, "--target", sha]
    tmp = None
    if notes:
        tmp = tempfile.NamedTemporaryFile("w", suffix=".md", delete=False, encoding="utf-8")
        tmp.write(notes)
        tmp.close()
        cmd += ["--notes-file", tmp.name]
    else:
        cmd += ["--generate-notes"]  # fall back to auto notes if the changelog has no section
    rc, out = _run(cmd, timeout=120)
    if tmp:
        try:
            os.unlink(tmp.name)
        except Exception:
            pass
    if rc == 0:
        print("pkg_firmware: published GitHub release %s with firmware attached" % tag)
    else:
        tail = out.strip().splitlines()[-1] if out.strip() else "gh error"
        print("pkg_firmware: release %s not published -- %s" % (tag, tail))


def _do_artifacts(verbose=False):
    version = _firmware_version()
    try:
        src_bin = os.path.join(env.subst("$BUILD_DIR"), "firmware.bin")  # noqa: F821
        os.makedirs("dist", exist_ok=True)
        dst = os.path.join("dist", "vibrashock-v%s.bin" % (version or "unknown"))
        shutil.copyfile(src_bin, dst)
        print("pkg_firmware: OTA image -> %s" % dst)
    except Exception as e:
        print("pkg_firmware: dist copy skipped (%s)" % e)
        return
    try:
        _maybe_release(version, dst, verbose=verbose)
    except Exception as e:  # a convenience step must never fail the build
        print("pkg_firmware: release step skipped (%s)" % e)


# Runs after firmware.bin is (re)built -- i.e. only on a real compile.
def _post_build(*args, **kwargs):
    _do_artifacts(verbose=False)


# Runs on `pio run -t release`, whether or not anything recompiled.
def _release_target(*args, **kwargs):
    _do_artifacts(verbose=True)


env.AddPostAction("$BUILD_DIR/firmware.bin", _post_build)  # noqa: F821
env.AddCustomTarget(  # noqa: F821
    name="release",
    dependencies="$BUILD_DIR/firmware.bin",
    actions=[_release_target],
    title="Publish release",
    description="Copy the OTA image to dist/ and publish a GitHub release when the version is new",
)
