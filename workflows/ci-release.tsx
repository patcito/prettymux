/**
 * Smithers workflow: PrettyMux CI/CD Release Pipeline
 *
 * Creates GitHub Actions workflows that build and release prettymux
 * for Linux (deb, rpm, snap, flatpak, AUR), Windows (exe/msi), and macOS (dmg).
 *
 * Usage:
 *   bunx smithers run workflows/ci-release.tsx
 *
 * Phase 1: Linux packaging (deb, rpm, Arch PKGBUILD, snap, flatpak)
 * Phase 2: Windows + macOS builds, GitHub Release automation
 *
 * Delete smithers.db before running to start fresh.
 */
import { createSmithers, Ralph, Sequence } from "smithers-orchestrator";
import { ClaudeCodeAgent, CodexAgent } from "smithers-orchestrator";
import { z } from "zod";

const implResult = z.object({
  summary: z.string(),
  filesChanged: z.array(z.string()),
});

const reviewResult = z.object({
  approved: z.boolean(),
  issues: z.array(z.string()),
  summary: z.string(),
});

const { Workflow, Task, smithers, outputs } = createSmithers({
  impl1: implResult,
  review1: reviewResult,
  impl2: implResult,
  review2: reviewResult,
});

const PROJECT_DIR = "/home/pe/newnewrepos/w/yo/prettymux";

const coder = new ClaudeCodeAgent({
  model: "claude-opus-4-6",
  dangerouslySkipPermissions: true,
  allowDangerouslySkipPermissions: true,
});

const reviewer = new CodexAgent({
  model: "gpt-5.3-codex",
  config: { model_reasoning_effort: "high" },
  dangerouslyBypassApprovalsAndSandbox: true,
  skipGitRepoCheck: true,
  cd: PROJECT_DIR,
});

export default smithers((ctx) => (
  <Workflow name="prettymux-ci-release">
    <Sequence>

      {/* ═══════════════════════════════════════════════════════════════
          Phase 1: Linux Packaging + GitHub Actions
          ═══════════════════════════════════════════════════════════════ */}
      <Ralph
        until={ctx.latest(reviewResult, "review1")?.approved}
        maxIterations={3}
        onMaxReached="return-last"
      >
        <Sequence>
        <Task id="impl1" output={implResult} agent={coder} retries={2}>
          {`You are working on PrettyMux at ${PROJECT_DIR}.
This is a GTK4 app built with CMake. Source: src/qt/main.cpp
It depends on: GTK4, libadwaita, WebKitGTK, libghostty, GLAD

The ghostty fork is at https://github.com/patcito/ghostty branch linux-embedded-platform
The prettymux repo is at https://github.com/patcito/prettymux

${ctx.latest(reviewResult, "review1")?.issues?.length ? '\nPREVIOUS REVIEW FEEDBACK:\n' + ctx.latest(reviewResult, "review1")!.issues.join("\n") + '\n' : ""}

Implement PHASE 1: Linux CI + Packaging

Create the following files:

1. .github/workflows/release.yml — Main release workflow
   Triggers: push to main, tags matching v*

   Jobs:

   a) build-linux-ubuntu-lts (ubuntu-22.04):
      - Checkout prettymux and ghostty fork (linux-embedded-platform branch)
      - Install: qt6-base-dev qt6-webengine-dev libgl-dev cmake zig
      - Build ghostty: cd ghostty && zig build -Dapp-runtime=none -Doptimize=ReleaseFast
      - Build prettymux: cd src/qt/build && cmake .. -DCMAKE_BUILD_TYPE=Release && make
      - Copy binary + welcome.html + shell integration + libghostty.so
      - Upload artifact

   b) build-linux-ubuntu-current (ubuntu-24.04):
      Same as above but on ubuntu-24.04

   c) build-linux-fedora (container: fedora:latest):
      - Install: qt6-qtbase-devel qt6-qtwebengine-devel mesa-libGL-devel cmake gcc-c++
      - Install zig from tarball
      - Build same way
      - Create RPM using nFPM (install nfpm)
      - Upload .rpm artifact

   d) build-linux-deb:
      - After ubuntu build, create .deb using nFPM
      - nfpm config in packaging/nfpm.yaml:
        name: prettymux
        arch: amd64
        version: from git tag or 0.1.0
        maintainer: patcito
        description: GPU-accelerated terminal multiplexer
        depends: [qt6-webengine, libgl1]
        contents:
          - src: prettymux -> dst: /usr/bin/prettymux
          - src: libghostty.so -> dst: /usr/lib/prettymux/libghostty.so
          - src: welcome.html -> dst: /usr/share/prettymux/welcome.html
          - src: prettymux-shell-integration.sh -> dst: /usr/share/prettymux/shell-integration.sh
          - src: prettymux.desktop -> dst: /usr/share/applications/prettymux.desktop
      - Upload .deb artifact

   e) build-snap:
      - Use snapcore/action-build@v1 to build snap
      - Create snap/snapcraft.yaml with core22 base, classic confinement
      - Use snapcore/action-publish@v1 to publish if SNAPCRAFT_STORE_CREDENTIALS secret set
      - Upload .snap artifact

   f) build-flatpak:
      - Use flatpak/flatpak-github-actions/flatpak-builder@v6 action
      - Create packaging/dev.prettymux.app.yml manifest with org.kde.Platform 6.6
      - The action handles flatpak-builder setup automatically
      - Upload .flatpak bundle artifact

   g) build-archlinux:
      - Use 2m/arch-pkgbuild-builder@v1 action to build in Arch container
      - Create packaging/PKGBUILD with proper source URLs and build steps
      - For AUR publishing use KSXGitHub/github-actions-deploy-aur@v3
        (requires AUR_SSH_PRIVATE_KEY secret)
      - Upload PKGBUILD artifact

   h) publish-ppa:
      - Use yuezk/publish-ppa-package@v2 action
      - Requires secrets: GPG_PRIVATE_KEY, GPG_PASSPHRASE, LAUNCHPAD_SSH_PRIVATE_KEY
      - Create packaging/debian/ directory with:
        control, changelog, rules, compat, copyright files
      - Target PPAs: ppa:patcito/prettymux
      - Only runs on version tags and if secrets are set

   i) build-appimage:
      - Use linuxdeploy to create AppImage
      - Use AppImageCrafters/build-appimage@v1 or manual linuxdeploy setup
      - Create packaging/AppImageBuilder.yml
      - Upload .AppImage artifact

2. packaging/nfpm.yaml — nFPM config for deb/rpm generation

3. packaging/prettymux.desktop — Desktop entry file:
   [Desktop Entry]
   Name=PrettyMux
   Comment=GPU-accelerated terminal multiplexer
   Exec=prettymux
   Icon=utilities-terminal
   Type=Application
   Categories=System;TerminalEmulator;

4. packaging/PKGBUILD — Arch Linux package build script

5. snap/snapcraft.yaml — Snap package config

6. packaging/dev.prettymux.app.yml — Flatpak manifest

echo "Phase 1: Creating Linux CI workflows..."
echo "Phase 1: Creating packaging configs..."

Return JSON: { summary: "...", filesChanged: ["path1", ...] }`}
        </Task>

        <Task id="review1" output={reviewResult} agent={reviewer} retries={2}>
          {`Review Phase 1 (Linux CI + Packaging) at ${PROJECT_DIR}.

Check:
1. .github/workflows/release.yml exists with all Linux jobs
2. Each job: checkout, install deps, build ghostty, build prettymux
3. packaging/nfpm.yaml exists for deb/rpm generation
4. packaging/prettymux.desktop exists
5. packaging/PKGBUILD exists for Arch Linux
6. snap/snapcraft.yaml exists
7. packaging/dev.prettymux.app.yml exists for Flatpak
8. Jobs upload artifacts
9. Workflow triggers on push to main and version tags
10. Correct GTK4 package names per distro

If ALL pass: { approved: true, issues: [], summary: "Phase 1 looks good" }
If ANY issue: { approved: false, issues: ["..."], summary: "Found N issues" }`}
        </Task>
        </Sequence>
      </Ralph>

      {/* ═══════════════════════════════════════════════════════════════
          Phase 2: Windows + macOS + Release Automation
          ═══════════════════════════════════════════════════════════════ */}
      <Ralph
        until={ctx.latest(reviewResult, "review2")?.approved}
        maxIterations={3}
        onMaxReached="return-last"
      >
        <Sequence>
        <Task id="impl2" output={implResult} agent={coder} retries={2}>
          {`You are working on PrettyMux at ${PROJECT_DIR}.
Read .github/workflows/release.yml created in Phase 1.

The ghostty fork: https://github.com/patcito/ghostty branch linux-embedded-platform
The prettymux repo: https://github.com/patcito/prettymux

${ctx.latest(reviewResult, "review2")?.issues?.length ? '\nPREVIOUS REVIEW FEEDBACK:\n' + ctx.latest(reviewResult, "review2")!.issues.join("\n") + '\n' : ""}

Implement PHASE 2: Windows + macOS + Release

Add to .github/workflows/release.yml:

1. build-windows (windows-latest):
   - Checkout prettymux and ghostty fork
   - Install Zig: use mlugg/setup-zig@v1 action
   - Install Qt6: use jurplel/install-qt-action@v4 with:
     version: '6.6.0'
     modules: 'qtwebengine'
     arch: 'win64_msvc2019_64'
   - Build ghostty for Windows:
     cd ghostty && zig build -Dapp-runtime=none -Doptimize=ReleaseFast -Dtarget=x86_64-windows
     (NOTE: This may not work yet since ghostty Windows support is limited.
      If it fails, add a comment explaining and skip the ghostty build.
      The job should still create the prettymux binary structure.)
   - Build prettymux with CMake + MSVC:
     cmake .. -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Release
     cmake --build . --config Release
   - Package with NSIS or just zip:
     Create a zip with prettymux.exe + Qt DLLs + libghostty.dll + welcome.html
     Use windeployqt to gather Qt dependencies
   - Upload artifact: prettymux-windows-x64.zip

2. build-macos (macos-14, ARM64):
   - Checkout prettymux and ghostty fork
   - Install Zig: use mlugg/setup-zig@v1
   - Install Qt6: use jurplel/install-qt-action@v4 with:
     version: '6.6.0'
     modules: 'qtwebengine'
   - Build ghostty:
     cd ghostty && zig build -Dapp-runtime=none -Doptimize=ReleaseFast
     (macOS embedded apprt already works, this is what cmux uses)
   - Update CMakeLists.txt to handle macOS:
     Add APPLE checks, link with Metal framework if needed
     Set MACOSX_BUNDLE properties
   - Build prettymux:
     cmake .. -DCMAKE_BUILD_TYPE=Release && make
   - Create .app bundle and .dmg:
     Use macdeployqt to create the .app bundle
     Use create-dmg or hdiutil to make a .dmg
   - Upload artifact: PrettyMux.dmg

3. build-macos-intel (macos-13, x86_64):
   Same as above but for Intel Macs

4. release job (needs: all build jobs):
   Only runs on version tags (v*)
   - Download all artifacts
   - Create GitHub Release using softprops/action-gh-release@v2
   - Upload all packages:
     prettymux-ubuntu-22.04-amd64.deb
     prettymux-ubuntu-24.04-amd64.deb
     prettymux-fedora-x86_64.rpm
     prettymux-archlinux-PKGBUILD.tar.gz
     prettymux-linux-x86_64.snap
     prettymux-linux-x86_64.flatpak
     prettymux-windows-x64.zip
     PrettyMux-arm64.dmg
     PrettyMux-x64.dmg
   - Generate changelog from git log since last tag
   - Mark as pre-release if tag contains "beta" or "alpha"

5. Update CMakeLists.txt for cross-platform support:
   - Add #ifdef guards for Linux-specific code in main.cpp:
     Port scanner (/proc/net/tcp) -> #ifdef __linux__
     notify-send -> #ifdef __linux__
     BASH_ENV -> platform check
   - Add macOS framework linking (Metal, Cocoa)
   - Add Windows resource file for icon

6. Create packaging/prettymux.rc — Windows resource file with icon

echo "Phase 2: Adding Windows and macOS builds..."
echo "Phase 2: Adding release automation..."

Return JSON: { summary: "...", filesChanged: ["path1", ...] }`}
        </Task>

        <Task id="review2" output={reviewResult} agent={reviewer} retries={2}>
          {`Review Phase 2 (Windows + macOS + Release) at ${PROJECT_DIR}. FINAL REVIEW.

Check:
1. Windows job uses jurplel/install-qt-action and MSVC
2. macOS ARM64 and Intel jobs exist
3. macOS uses macdeployqt for .app bundle
4. Release job creates GitHub Release on version tags
5. All artifacts uploaded to release
6. CMakeLists.txt has platform checks
7. main.cpp Linux-specific code guarded with #ifdef
8. packaging/prettymux.rc exists for Windows
9. Changelog generated from git log
10. Pre-release flag for beta/alpha tags

If ALL pass: { approved: true, issues: [], summary: "Phase 2 complete" }
If ANY issue: { approved: false, issues: ["..."], summary: "Found N issues" }`}
        </Task>
        </Sequence>
      </Ralph>

    </Sequence>
  </Workflow>
));
