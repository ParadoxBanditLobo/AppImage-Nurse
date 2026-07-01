# AppImage-Nurse

AppImage Nurse is a small terminal toolkit for inspecting, troubleshooting, extracting, testing, and repacking AppImages. It is not a universal AppImage fixer; it is a companion tool that helps explain problems, guide repairs, and streamline common AppImage workflows.

## Features

- Diagnose AppImages and AppDirs
- Extract to disk or RAM workspace
- Repack with appimagetool
- Test launch and explain common errors
- Generate reports and session history
- Smart dependency bundling attempts
- RPATH patching with patchelf
- Framework checks for Qt, GTK, Electron, SDL/Raylib, Python, and Godot/Redot
- First Aid guided workflows
- Tool path manager
- Guided AppDir creation

## Requirements

Core diagnosis works without extra tools.

Optional backend tools:
- appimagetool: repacking AppDirs into AppImages
- patchelf: RPATH/RUNPATH patching
- linuxdeploy: stronger dependency bundling
- Docker or Podman: compatibility build/testing workflows
- appstreamcli: AppStream metadata checks
- (separate packages have their own licenses)

## Usage

Download the release zip, extract it, then run:

chmod +x appimage-nurse
./appimage-nurse

Note: AppImage Nurse itself is not currently shipped as an AppImage. It may be packaged as one separately.

## Scope

AppImage Nurse is a diagnostic and companion tool. It can repair simple packaging problems and guide common workflows, but some issues require rebuilding the original application, especially glibc compatibility problems.
This is not a miracle worker, but meant to streamline troubleshooting.

## Full-Disclosure
Ai was used to create the code for this application

## Build from source

```sh
chmod +x build.sh
./build.sh
