#!/bin/sh
set -eu
cc=${CC:-gcc}
$cc -std=c11 -O2 -static -s -o appimage-nurse src/appimage_nurse.c
