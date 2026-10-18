# PlatformIO custom targets (extra_scripts = post:tools/pio_targets.py).
#
#   .\tools\pio.ps1 run -e xiao_c5 -t publish_release      stable vX.Y.Z tag
#   .\tools\pio.ps1 run -e xiao_c5 -t publish_prerelease   vX.Y.Z-beta.N tag
#
# (or the VS Code tasks).  Pass -e: without it every environment that
# loads this script would run it.
#
# Both run tools/publish_release.py, which checks the tree, tags and pushes;
# .github/workflows/release.yml then builds and publishes the GitHub Release
# that the RX58_ESP32C5 Flasher downloads.  The workflow keys off a `-` in
# the tag to set GitHub's Pre-release flag.

import os
import subprocess
import sys

Import("env")  # noqa: F821  (SCons)


def _run_publish_script(env, prerelease=False):
    script = os.path.join(env.subst("$PROJECT_DIR"), "tools", "publish_release.py")
    # `-u`: unbuffered, so prompts appear before PIO's pipe flushes.
    args = [env.subst("$PYTHONEXE"), "-u", script]
    if prerelease:
        args.append("--prerelease")
    # Inherit stdin/stdout/stderr so the user sees prompts and can answer them.
    result = subprocess.run(args)
    if result.returncode != 0:
        sys.exit(result.returncode)   # PIO marks the target as failed


def publish_release(source, target, env):
    _run_publish_script(env, prerelease=False)


def publish_prerelease(source, target, env):
    _run_publish_script(env, prerelease=True)


env.AddCustomTarget(  # noqa: F821
    name="publish_release",
    dependencies=None,
    actions=publish_release,
    title="Publish Release (tag + push)",
    description="Tag a stable vMAJOR.MINOR.PATCH and push; CI publishes a full GitHub Release",
)

env.AddCustomTarget(  # noqa: F821
    name="publish_prerelease",
    dependencies=None,
    actions=publish_prerelease,
    title="Publish Pre-release (beta tag + push)",
    description="Tag with a -beta.N suffix and push; CI publishes with the Pre-release flag set",
)
