# SPDX-License-Identifier: MIT-0
# SPDX-FileCopyrightText: 2026 Skye Soss
{ lib, ... }:
{
  name = "qemu-bridge-helper";

  nodes.machine =
    { pkgs, ... }:
    {
      imports = [ ./nixos-module.nix ];

      services.qemu-bridge-helper.enable = true;

      virtualisation.libvirtd = {
        enable = true;
        allowedBridges = [
          "br0"
          "nonexistent"
        ];
      };

      networking.bridges = {
        br0.interfaces = [ ];
        br1.interfaces = [ ];
      };

      users.users.alice.isNormalUser = true;

      environment.systemPackages = [
        # Usage: with-tap [--use-vnet] BRIDGE CMD...
        # Gets a tap device from qemu-bridge-helper the way qemu does, then
        # runs CMD with the device's name in $TAP while holding its fd open.
        (pkgs.writers.writePython3Bin "with-tap" { } ''
          import os
          import socket
          import subprocess
          import sys

          args = sys.argv[1:]
          flags = []
          if args[0] == "--use-vnet":
              flags.append(args.pop(0))
          bridge, *cmd = args

          ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
          fd = theirs.fileno()
          helper = subprocess.run(
              ["qemu-bridge-helper", *flags, f"--br={bridge}", f"--fd={fd}"],
              pass_fds=[fd],
          )
          theirs.close()
          if helper.returncode != 0:
              sys.exit(helper.returncode)

          _, fds, _, _ = socket.recv_fds(ours, 1, 1)
          with open(f"/proc/self/fdinfo/{fds[0]}") as f:
              tap = next(ln.split()[1] for ln in f if ln.startswith("iff:"))
          print(f"got {tap}", file=sys.stderr)

          sys.exit(subprocess.run(cmd, env={**os.environ, "TAP": tap}).returncode)
        '')
      ];
    };

  testScript =
    { nodes, ... }:
    let
      inherit (nodes.machine.virtualisation.libvirtd.qemu) package;
      qemu = lib.getExe' package "qemu-system-${package.stdenv.hostPlatform.qemuArch}";
    in
    ''
      def as_alice(cmd):
          return f"runuser -u alice -- {cmd}"

      def no_taps():
          machine.fail("ip -o link show type tun | grep tap")

      machine.wait_for_unit("multi-user.target")
      machine.wait_for_unit("qemu-bridge-helper.socket")
      machine.wait_for_unit("br0-netdev.service")
      machine.wait_for_unit("br1-netdev.service")

      with subtest("Wrapper runs our helper without setuid"):
          machine.succeed(
              "test ! -u /run/wrappers/bin/qemu-bridge-helper",
              "test $(runuser -u alice -- sh -c 'command -v qemu-bridge-helper')"
              " = /run/wrappers/bin/qemu-bridge-helper",
          )

      with subtest("Server is reachable via Varlink"):
          machine.succeed(as_alice(
              "varlinkctl introspect /run/varlink/registry/website.soss.QemuBridgeHelper"
              " website.soss.QemuBridgeHelper | grep CreateTap"
          ))

      with subtest("Tap device is created on an allowed bridge"):
          machine.succeed(as_alice("""with-tap br0 sh -euxc '
              ip -o link show master br0 up | grep -F " $TAP:"
              grep -i ^fe: /sys/class/net/$TAP/address
              test "$(cat /sys/class/net/$TAP/mtu)" = "$(cat /sys/class/net/br0/mtu)"
              test $(( $(cat /sys/class/net/$TAP/tun_flags) & 0x4000 )) = 0
          '"""))
          # The device goes away with the fd
          no_taps()

      with subtest("--use-vnet enables virtio-net headers"):
          machine.succeed(as_alice("""with-tap --use-vnet br0 sh -euxc '
              test $(( $(cat /sys/class/net/$TAP/tun_flags) & 0x4000 )) != 0
          '"""))
          no_taps()

      with subtest("Bridges not in the ACL are rejected"):
          out = machine.fail(as_alice("with-tap br1 true 2>&1"))
          assert "website.soss.QemuBridgeHelper.BridgeNotAllowed" in out, out
          no_taps()

      with subtest("Allowed bridges that don't exist are rejected"):
          out = machine.fail(as_alice("with-tap nonexistent true 2>&1"))
          assert "website.soss.QemuBridgeHelper.NoSuchBridge" in out, out
          no_taps()

      # libvirt passes this helper path to QEMU
      with subtest("QEMU can use it as its bridge helper"):
          machine.succeed(as_alice(
              "${qemu} -machine none -nodefaults -display none"
              " -netdev bridge,id=n0,br=br0,helper=/run/wrappers/bin/qemu-bridge-helper"
              " -daemonize -pidfile /tmp/qemu.pid"
          ))
          machine.succeed("ip -o link show master br0 up | grep tap")
          machine.succeed("kill $(cat /tmp/qemu.pid)")
          machine.wait_until_succeeds("! ip -o link show type tun | grep tap")
    '';
}
