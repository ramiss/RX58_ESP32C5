"""
Publish a new RX58_ESP32C5 release.

Invoked from PlatformIO via two custom targets that share this script
(tools/pio_targets.py):

  * `publish_release`     — stable release.  Tag is `vMAJOR.MINOR.PATCH`.
                             CI publishes a full GitHub Release, which the
                             RX58_ESP32C5 Flasher lists as the latest.

  * `publish_prerelease`  — beta / RC.  Tag is `vMAJOR.MINOR.PATCH-suffix`
                             (e.g. v0.1.1-beta.1).  CI publishes with the
                             "Pre-release" checkbox ticked, which excludes it
                             from /releases/latest; the flasher shows it only
                             with "Include pre-releases" checked.

  .\\tools\\pio.ps1 run -e xiao_c5 -t publish_release      (or the VS Code task)

The script walks the user through:

  1. Verifying the working tree is clean and on a sensible branch.
  2. Verifying the GitHub remote is configured.
  3. Suggesting the next version: MAJOR.MINOR always comes from
     RX_FW_VERSION in src/receiver.h (the version hosts read from the INFO
     block), and CI refuses a tag that disagrees with it.
  4. Confirming the version + a one-line tag annotation.
  5. Creating an annotated tag and pushing it to origin.

Once the tag is on origin, .github/workflows/release.yml takes over: it
builds the firmware, attaches the merged image, the app image and
flash-manifest.json, and publishes the GitHub Release with a link to the
flasher.  The workflow detects the `-` in the tag name and sets the
Pre-release flag automatically for prerelease tags.

Mode selection:
  * --prerelease flag on the command line, or
  * RELEASE_PRERELEASE=1 in the environment.

Non-interactive use:
    set RELEASE_VERSION=v0.1.0
    set RELEASE_NOTES="First release"
    .\\tools\\pio.ps1 run -e xiao_c5 -t publish_release
"""

import os
import re
import subprocess
import sys
from pathlib import Path

# When invoked via PIO's subprocess machinery, stdout/stderr are pipes (not a
# TTY), and Python switches to block-buffering.  Force line buffering so every
# print() flushes before the input() prompts it explains.
try:
    sys.stdout.reconfigure(line_buffering=True)
    sys.stderr.reconfigure(line_buffering=True)
except (AttributeError, ValueError):
    pass

REPO_ROOT = Path(__file__).resolve().parent.parent
PRODUCT = "RX58_ESP32C5"
FW_VERSION_FILE = REPO_ROOT / "src" / "receiver.h"
FLASHER_URL = "https://github.com/ramiss/RX58_ESP32C5-Flasher/releases/latest"

# Tag shape regexes.  The CI workflow keys off the presence of a `-` in the
# tag name to set the GitHub "Pre-release" flag.
STABLE_TAG_RE     = re.compile(r"^v\d+\.\d+\.\d+$")
PRERELEASE_TAG_RE = re.compile(r"^v\d+\.\d+\.\d+-[A-Za-z0-9][A-Za-z0-9.\-]*$")


def _is_prerelease_mode():
    """Return True when the user invoked the prerelease flow."""
    if "--prerelease" in sys.argv:
        return True
    return os.environ.get("RELEASE_PRERELEASE", "").strip().lower() in ("1", "true", "yes", "y")


def _run(cmd, capture=True, check=True, **kwargs):
    """Run a git command in the repo root.  Returns trimmed stdout when capturing."""
    result = subprocess.run(
        cmd,
        cwd=str(REPO_ROOT),
        capture_output=capture,
        text=True,
        check=check,
        **kwargs,
    )
    return result.stdout.strip() if capture else ""


def _fail(msg, code=1):
    print(f"\n[ERROR] {msg}", file=sys.stderr)
    sys.exit(code)


def _confirm(prompt, default_no=True):
    yn = "[y/N]" if default_no else "[Y/n]"
    # Emit the prompt as a complete line so PIO's task runner flushes it, then
    # read on the next line via a `>` continuation marker.
    print(f"{prompt} {yn}", flush=True)
    sys.stdout.write("> ")
    sys.stdout.flush()
    try:
        ans = input().strip().lower()
    except (EOFError, KeyboardInterrupt):
        print("\n[ABORTED] No input received.")
        sys.exit(1)
    if not ans:
        return not default_no
    return ans in ("y", "yes")


def _fw_version():
    """(major, minor) from RX_FW_VERSION in src/receiver.h (major * 256 + minor)."""
    text = FW_VERSION_FILE.read_text(encoding="utf-8")
    m = re.search(r"#define\s+RX_FW_VERSION\s+(0x[0-9A-Fa-f]+|\d+)", text)
    if not m:
        _fail(f"RX_FW_VERSION not found in {FW_VERSION_FILE.relative_to(REPO_ROOT)}")
    raw = int(m.group(1), 0)
    return raw >> 8, raw & 0xFF


def _assert_clean_tree():
    porcelain = _run(["git", "status", "--porcelain"])
    if not porcelain:
        return
    print("\n[ERROR] Working tree has uncommitted changes:\n")
    for line in porcelain.splitlines():
        print(f"  {line}")
    print(
        "\nThe release will be built from the tagged commit, so dirty state "
        "would ship something different from what you have locally.\n"
        "Commit, stash, or discard these changes before publishing."
    )
    sys.exit(1)


def _assert_remote():
    try:
        url = _run(["git", "remote", "get-url", "origin"])
    except subprocess.CalledProcessError:
        _fail("No `origin` git remote configured. Add one with: git remote add origin <url>")
    if "github.com" not in url:
        print(f"\n[WARN] origin is '{url}', which doesn't look like GitHub.")
        print("       The release workflow expects a GitHub remote.")
        if not _confirm("Continue anyway?"):
            sys.exit(1)
    return url


def _assert_pushed(branch):
    """The tag is built by CI from origin, so HEAD must already be there."""
    try:
        _run(["git", "fetch", "-q", "origin", branch], timeout=30)
        behind_ahead = _run(["git", "rev-list", "--left-right", "--count", f"origin/{branch}...HEAD"])
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        print("[WARN] Could not compare with origin — make sure your commits are pushed.")
        return
    ahead = int(behind_ahead.split()[1])
    if ahead:
        _fail(f"{ahead} local commit(s) on '{branch}' are not on origin yet.\n"
              f"Push them first (git push origin {branch}) so the tag points at a commit CI can see.")


def _current_branch():
    return _run(["git", "branch", "--show-current"])


def _latest_tag():
    """Return the most recent vX.Y.Z tag, or None if no matching tag exists."""
    try:
        return _run(["git", "describe", "--tags", "--abbrev=0", "--match=v*.*.*"], check=True)
    except subprocess.CalledProcessError:
        return None


def _suggest_next_version(current, fw, prerelease=False):
    """Suggest the next tag.  MAJOR.MINOR always follows RX_FW_VERSION (`fw`).

    Stable mode:
      v0.1.1            -> v0.1.2          (next patch, same firmware version)
      v0.1.2-beta.3     -> v0.1.2          (finalise the beta as the stable patch)
      v0.1.4, fw 0.2    -> v0.2.0          (RX_FW_VERSION was bumped)
      (no prior tag)    -> v0.1.0          (for fw 0.1)

    Prerelease mode:
      v0.1.1            -> v0.1.2-beta.1   (start a beta cycle for the next patch)
      v0.1.2-beta.3     -> v0.1.2-beta.4   (continue the existing beta cycle)
      v0.1.4, fw 0.2    -> v0.2.0-beta.1
      (no prior tag)    -> v0.1.0-beta.1   (for fw 0.1)
    """
    maj, mn = fw
    fresh = f"v{maj}.{mn}.0-beta.1" if prerelease else f"v{maj}.{mn}.0"
    if not current:
        return fresh

    base = current.split("-", 1)[0]
    suffix = current[len(base) + 1:] if "-" in current else ""
    base_m = re.match(r"^v(\d+)\.(\d+)\.(\d+)$", base)
    if not base_m or (int(base_m.group(1)), int(base_m.group(2))) != fw:
        return fresh
    pat = int(base_m.group(3))

    cycle_m = re.match(r"^(beta|rc|alpha|pre)\.(\d+)$", suffix) if suffix else None
    if prerelease:
        if cycle_m:
            return f"v{maj}.{mn}.{pat}-{cycle_m.group(1)}.{int(cycle_m.group(2)) + 1}"
        return f"v{maj}.{mn}.{pat + 1}-beta.1"
    if cycle_m:
        return f"v{maj}.{mn}.{pat}"
    return f"v{maj}.{mn}.{pat + 1}"


def _assert_tag_unused(tag):
    try:
        _run(["git", "rev-parse", "--verify", f"refs/tags/{tag}"])
        _fail(
            f"Tag {tag} already exists locally.\n"
            f"Pick a different version, or delete the existing tag with:\n"
            f"  git tag -d {tag}"
        )
    except subprocess.CalledProcessError:
        pass  # not found — good

    # Remote (best effort — skip if network is down)
    try:
        result = subprocess.run(
            ["git", "ls-remote", "--tags", "origin", tag],
            cwd=str(REPO_ROOT), capture_output=True, text=True, timeout=15,
        )
        if result.returncode == 0 and result.stdout.strip():
            _fail(
                f"Tag {tag} already exists on origin.\n"
                f"Pick a different version.  If you need to replace it:\n"
                f"  git push origin :refs/tags/{tag}\n"
                f"  (then delete the corresponding GitHub Release manually)"
            )
    except (subprocess.TimeoutExpired, FileNotFoundError):
        print("[WARN] Could not reach origin to check for tag collision — letting push catch it.")


def _extract_owner_repo(remote_url):
    """Pull (owner, repo) out of a GitHub URL.  Returns (None, None) if unrecognised."""
    m = re.search(r"github\.com[:/]([^/]+)/([^/.]+?)(?:\.git)?/?$", remote_url)
    if not m:
        return None, None
    return m.group(1), m.group(2)


def main():
    prerelease = _is_prerelease_mode()
    mode_label = "Pre-release" if prerelease else "Release"
    print(f"\n=== {PRODUCT} {mode_label} Publisher ===\n")
    if prerelease:
        print("Pre-release mode: tag will use a -beta.N suffix and the GitHub")
        print("Release will be flagged Pre-release (excluded from /releases/latest).\n")

    print("[1/7] Working tree clean?")
    _assert_clean_tree()
    print("      OK.")

    print("[2/7] Origin remote configured?")
    remote_url = _assert_remote()
    print(f"      {remote_url}")

    branch = _current_branch()
    if not branch:
        _fail("HEAD is detached.  Check out a branch first (git switch main).")
    if branch not in ("main", "master"):
        print(f"\n[WARN] You are on branch '{branch}', not 'main'.")
        print("       Releases are usually cut from main — double-check this is intentional.")
        if not _confirm("Tag this branch and publish?"):
            sys.exit(1)

    print(f"[3/7] '{branch}' pushed to origin?")
    _assert_pushed(branch)
    print("      OK.")

    fw = _fw_version()
    latest = _latest_tag()
    suggested = _suggest_next_version(latest, fw, prerelease=prerelease)
    print(f"[4/7] RX_FW_VERSION: {fw[0]}.{fw[1]}   Latest tag: {latest or '(none)'}")
    print(f"      Suggested:  {suggested}")

    version_env = os.environ.get("RELEASE_VERSION", "").strip()
    if version_env:
        version = version_env
        print(f"      Using RELEASE_VERSION={version} from environment.")
    else:
        print()
        print(f"Version to publish [{suggested}]:", flush=True)
        sys.stdout.write("> ")
        sys.stdout.flush()
        try:
            user_in = input().strip()
        except (EOFError, KeyboardInterrupt):
            print("\nAborted.")
            sys.exit(1)
        version = user_in or suggested

    if prerelease:
        if not PRERELEASE_TAG_RE.match(version):
            _fail(
                f"Invalid pre-release version '{version}'.\n"
                "Use vMAJOR.MINOR.PATCH-suffix (e.g. v0.1.1-beta.1, v1.0.0-rc.2).\n"
                "Pre-release tags must include a `-` after the version triple — that's\n"
                "what tells CI to flag the GitHub Release as a Pre-release."
            )
    else:
        if not STABLE_TAG_RE.match(version):
            if "-" in version:
                _fail(
                    f"'{version}' looks like a pre-release tag (contains `-`).\n"
                    "If that's intentional, run the publish_prerelease target instead\n"
                    "(or set RELEASE_PRERELEASE=1)."
                )
            _fail(
                f"Invalid version '{version}'.  Use vMAJOR.MINOR.PATCH (e.g. v0.1.0).\n"
                "The CI workflow's tag filter is `v*.*.*`, so anything else won't trigger it."
            )

    # The same check CI makes ("Check version"): failing here costs nothing,
    # failing there leaves a pushed tag with no release.
    tag_mm = tuple(int(x) for x in re.match(r"^v(\d+)\.(\d+)", version).groups())
    if tag_mm != fw:
        _fail(
            f"{version} is {tag_mm[0]}.{tag_mm[1]}, but RX_FW_VERSION in src/receiver.h is "
            f"{fw[0]}.{fw[1]}.\nHosts read RX_FW_VERSION from the INFO block, so the tag must match it.\n"
            f"Bump RX_FW_VERSION (and commit + push) for a new minor version, or tag v{fw[0]}.{fw[1]}.N."
        )

    print(f"[5/7] Checking {version} is unused...")
    _assert_tag_unused(version)
    print("      OK.")

    notes_env = os.environ.get("RELEASE_NOTES", "").strip()
    if notes_env:
        notes = notes_env
    else:
        print()
        print("One-line tag annotation (or press Enter to use the version as the message).")
        print("This becomes the tag message; GitHub auto-generates richer release notes from")
        print("the commits since the previous tag.")
        sys.stdout.write("> ")
        sys.stdout.flush()
        try:
            notes_in = input().strip()
        except (EOFError, KeyboardInterrupt):
            print("\nAborted.")
            sys.exit(1)
        notes = notes_in or version

    print()
    print("=" * 60)
    print(f"  Mode:    {mode_label}{' (GitHub Pre-release flag will be set)' if prerelease else ''}")
    print(f"  Branch:  {branch}")
    print(f"  Tag:     {version}")
    print(f"  Message: {notes}")
    print(f"  Remote:  {remote_url}")
    print("=" * 60)
    print()
    if not _confirm("Proceed?"):
        print("Aborted.")
        sys.exit(0)

    print(f"\n[6/7] Creating annotated tag {version}...")
    _run(["git", "tag", "-a", version, "-m", notes])
    print("      Done.")

    print(f"[7/7] Pushing {version} to origin...")
    try:
        subprocess.run(["git", "push", "origin", version], cwd=str(REPO_ROOT), check=True)
    except subprocess.CalledProcessError:
        print("\n[ERROR] Push failed.  Removing the local tag so you can retry cleanly.")
        try:
            _run(["git", "tag", "-d", version])
        except subprocess.CalledProcessError:
            pass
        sys.exit(1)

    print()
    print("=" * 60)
    print("  SUCCESS")
    print("=" * 60)
    print(f"  Tag {version} pushed.  GitHub Actions is now building.")
    print()
    owner, repo = _extract_owner_repo(remote_url)
    if owner and repo:
        print("  Watch the build:")
        print(f"    https://github.com/{owner}/{repo}/actions")
        print()
        print("  When green, the release will appear at:")
        print(f"    https://github.com/{owner}/{repo}/releases/tag/{version}")
    print()
    print(f"  Flash it with the {PRODUCT} Flasher ({FLASHER_URL}).")
    print()


if __name__ == "__main__":
    main()
