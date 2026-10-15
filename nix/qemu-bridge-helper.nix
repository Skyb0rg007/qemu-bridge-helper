# SPDX-License-Identifier: MIT-0
# SPDX-FileCopyrightText: 2026 Skye Soss
{
  pkgs ? import <nixpkgs> { },
}:
let
  inherit (pkgs) lib;
in
pkgs.stdenv.mkDerivation {
  pname = "qemu-bridge-helper";
  version = "0.1.0";

  src = lib.cleanSource ./..;

  nativeBuildInputs = [ pkgs.pkg-config ];

  buildInputs = [ pkgs.systemdLibs ];

  makeFlags = [ "prefix=$(out)" ];

  meta = {
    description = "Implementation of qemu-bridge-helper without setuid";
    homepage = "https://github.com/Skyb0rg007/qemu-bridge-helper";
    license = lib.licenses.gpl2Plus;
  };
}
