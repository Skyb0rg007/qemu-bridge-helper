# SPDX-License-Identifier: MIT-0
# SPDX-FileCopyrightText: 2026 Skye Soss
{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  nativeBuildInputs = [
    pkgs.pkg-config
    pkgs.reuse
  ];
  buildInputs = [
    pkgs.systemdLibs
  ];
}
