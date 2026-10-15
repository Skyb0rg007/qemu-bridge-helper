# SPDX-License-Identifier: MIT-0
# SPDX-FileCopyrightText: 2026 Skye Soss
{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.services.qemu-bridge-helper;
in
{
  options.services.qemu-bridge-helper = {
    enable = lib.mkEnableOption "the socket-activated qemu-bridge-helper server";

    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.callPackage ./qemu-bridge-helper.nix { };
      description = "The qemu-bridge-helper package to use.";
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [ cfg.package ];

    systemd.packages = [ cfg.package ];
    systemd.sockets.qemu-bridge-helper.wantedBy = [ "sockets.target" ];

    security.wrappers.qemu-bridge-helper = {
      source = lib.mkForce "${cfg.package}/bin/qemu-bridge-helper";
      setuid = lib.mkForce false;
      owner = "root";
      group = "root";
    };
  };
}
