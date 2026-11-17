# natartos/tools

Standalone utilities for the goggles, separate from the mod framework.

## dm_noverity

Swaps the dm-verity target on `/system` for a dm-linear target at runtime, so
`/system` can be remounted rw for the current session. Runtime only — a reboot
restores dm-verity, and the tool itself is safe to run. Used by
`toolya natartos noverity`.

**Do not modify `/system` and reboot** — dm-verity mode 2 bricks the device on
the next boot unless the hash tree is recalculated, and no tool for that exists.

Build: static aarch64 (`aarch64-linux-gnu-gcc -static -O2 -o dm_noverity tools/dm_noverity.c`); CI builds it.

## sepolicy_setperm

Marks one domain permissive in a binary SELinux policy (additive — only ever
allows more, never denies). Used to make `vendor_init` permissive in
`/vendor/etc/selinux/precompiled_sepolicy` so a boot-time service can run on
the DJI Goggles 3 (its policy ships with zero permissive domains and boots
enforcing). Never touches `/system`.

Build (host): `gcc -O2 -o sepolicy_setperm tools/sepolicy_setperm.c /usr/lib/x86_64-linux-gnu/libsepol.a`
Usage: `sepolicy_setperm <in-policy> <out-policy> <domain>`

The patched policy is runtime-load-tested (`> /sys/fs/selinux/load`) before it
is ever persisted, so a rejected policy can't bootloop the device.
