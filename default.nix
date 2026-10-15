# SPDX-License-Identifier: MIT-0
# SPDX-FileCopyrightText: 2026 Skye Soss
{
  pkgs ? import <nixpkgs> { },
}:
{
  qemu-bridge-helper = pkgs.callPackage ./nix/qemu-bridge-helper.nix { };

  nixosModules.default = ./nix/nixos-module.nix;

  tests.nixos = pkgs.testers.runNixOSTest ./nix/nixos-test.nix;
}
