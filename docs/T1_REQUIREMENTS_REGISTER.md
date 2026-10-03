# T1 and v1 requirements register (2026-10-03, rev 2)

Decisions and PR ownership are in `docs/OPERATOR_PLAN_2026-09-29.md`. Tags: **[REQUIRED]** required by the approved scope (T1 scope and the decisions in the plan); **[PROPOSED]** not approved, so not decided; **[VERIFIED]** read from this repo or measured; **[QUESTION]** the implementer must answer from the code; **[EXTERNAL]** a public source (section 8); **[INFERENCE]** my reasoning, not verified. Milestones: T1 (VMware test ISO), v1 (release), POST (after release).

## 1. Ports and connectors: what each one needs

A port is not a driver. The driver belongs to the controller behind it.

| Port | What the OS needs | Status | Milestone |
|---|---|---|---|
| HDMI, DisplayPort, USB-C video | A driver for the GPU the port is wired to. DP Alt Mode over USB-C is a hardware feature [EXTERNAL 1,2]. | GOP framebuffer only, verified on OVMF only (#15). No native GPU driver. | T1 (VMware) / POST (native) |
| HDMI and DP audio | A HD Audio controller driver plus the display-audio codec [EXTERNAL 3]. | No audio stack seen. | v1 |
| Ethernet (RJ45) | A NIC driver. Common real chips are Intel I225/I226 and Realtek RTL81xx [EXTERNAL 4]. | Only virtio-net and e1000e. | T1: e1000e; v1: Realtek and Intel 2.5GbE |
| USB-A and USB-C data | xHCI host controller, hub, HID class, mass storage, UVC webcam. | No USB code (#16). | T1 for HID; v1 for storage |
| USB-C power and roles | UCSI over ACPI, or a Type-C port-controller driver [EXTERNAL 5,6]. | None. | POST |
| Thunderbolt and USB4 | A controller driver plus PCIe tunnelling and security levels. | None. | POST |
| Audio jack, speakers, mic | HD Audio controller and codec drivers. | None seen. | v1 |
| SD card reader | Usually a USB or PCIe card-reader driver. | None. | POST |
| Built-in keyboard, trackpad | i8042/PS/2 is covered; many laptop trackpads and touchscreens are I2C-HID behind ACPI [EXTERNAL 7]. | PS/2 only. | T1 (I2C-HID) |
| Wi-Fi and Bluetooth | Per-chipset drivers and firmware (#38, #39). | None. | POST |

[INFERENCE from EXTERNAL 8,9] On laptops with Intel or AMD plus NVIDIA graphics, HDMI and DisplayPort are often wired to the NVIDIA GPU. The operator's LG G-Sync monitor points the same way. Without a vendor GPU driver the OS then cannot light that port on real hardware. Verify the Legion's port wiring before promising real-hardware external display. VMware avoids this because it presents a virtual display.

## 2. Touch and multi-display [REQUIRED]

The operator owns no touch hardware, so T1 validates touch on emulated devices.

- [VERIFIED by Claude Code, QEMU 8.2.2] The emulated devices available are `virtio-multitouch-pci`, `virtio-tablet-pci`, and the USB devices `usb-tablet` and `usb-wacom-tablet` on the `qemu-xhci` bus.
- [VERIFIED by Claude Code] Today `virtio_input.c` folds only `ABS_X` and `ABS_Y`, with no `ABS_MT_*` slot handling, and the kernel has no xHCI driver.
- Multi-touch checks (R-T1, R-T3) use `virtio-multitouch-pci` first, because it needs no USB stack. `usb-tablet` is an absolute pointer and is used for pointer tests only; use it after xHCI lands. Real I2C-HID digitizers, vendor HID descriptors and precision-touchpad gestures still need real hardware.

- R-T1 (T1): xHCI plus USB HID, including the multi-touch digitizer report format (contact id, position, pressure) [EXTERNAL 10].
- R-T2 (T1): I2C-HID via ACPI for laptop-internal touchscreens and precision trackpads [EXTERNAL 7].
- R-T3 (T1): a multi-contact input abstraction (`ABS_MT_*` slots) with tap, drag and pinch, palm rejection, and an on-screen keyboard.
- R-T4 (T1): UI hit targets and layouts that work for both finger and pointer, with both usable at once.
- R-T5 (v1): bind each touch device to the display it belongs to, with per-display calibration [EXTERNAL 11]. A touch monitor on the second output drives only that output.
- R-T6 (POST): validate on a real USB touch monitor once one is available.
- R-D1 (T1): read EDID; support more than one scanout (today one scanout and a 1024x768 fallback, #55).
- R-D2 (T1): hotplug detection, extended and mirrored modes, per-monitor resolution.
- R-D3 (v1): per-monitor scaling from EDID physical size and native resolution [EXTERNAL 12,13].
- R-D4 (T1): document exactly how a second display behaves with GOP only, and what needs a native GPU driver.
- R-D5 (POST): refresh-rate selection (for example 100 Hz) and variable refresh (G-Sync, FreeSync); both need native modesetting.

## 3. Requirements register: components that may have been missed

| ID | Requirement | Milestone |
|---|---|---|
| R-01 | Audio: HD Audio controller, codec, mixer, per-app volume, HDMI audio | v1 |
| R-02 | Accessibility: keyboard-only operation, screen reader, high contrast, large text, sticky keys | v1 |
| R-03 | Text: Unicode fonts, input methods (IME), non-US keyboard layouts (#56), emoji fallback | v1 |
| R-04 | Locale and time: time zone, daylight saving, NTP time sync | v1 |
| R-05 | Users and permissions: accounts, or a documented single-owner model (DDR-1143 says no accounts) | v1 |
| R-06 | Power: ACPI battery and AC status through the embedded controller, lid switch, brightness keys, suspend and shutdown behaviour [EXTERNAL 14] | v1 |
| R-07 | Thermal and fan reporting (#54) | POST |
| R-08 | USB mass storage and hot-unplug safety (flush before removal) | v1 |
| R-09 | Webcam (USB UVC) and microphone | POST |
| R-10 | Clipboard, drag and drop, file manager, settings app, text editor | T1: file manager and settings; rest v1 |
| R-11 | Browser or web view with a TLS certificate store | v1 |
| R-12 | DHCP and DNS (#43), IPv6, a firewall, and network settings UI | T1: DHCP and DNS; rest v1 |
| R-13 | Signed update mechanism and rollback (#20); signed release ISO and published checksums | v1 |
| R-14 | Crash reporting that never includes key material; persistent log (#41, #42) | v1 |
| R-15 | Firmware update awareness and TPM drift recovery | v1 |
| R-16 | Print and scan support | POST |
| R-17 | Licence and third-party notices (#37); FIPS wording that never claims certification (#80) | v1 |
| R-18 | Release notes that state every deferred item, the QEMU-only testing history (#15), and the OPEN-1 status | v1 |
| R-19 | Hardware compatibility list and real-hardware bring-up on at least one UEFI machine | v1 |
| R-20 | Localization of installer and UI strings | POST |
| R-21 | GPU acceleration for the compositor | POST |
| R-22 | Developer tooling: serial console, debug shell, diagnostic bundle export | v1 |

## 4. Install, encryption and key safety [REQUIRED]

Order: DDR-1144 (crypt device, KDF per plan decision D5, keyslots, known-answer tests), then DDR-1146 (recovery key), then DDR-1145 (TPM+PIN). Keep the project's gates and mutation checks.

Answers read from DDR-1144 by Claude Code (DDR-1144 is a design only, no code yet) [VERIFIED as a design]:
- Mode: AEAD (ChaCha20-Poly1305). The nonce is 96 random bits per write from a per-volume ChaCha20 DRBG, stored with the tag as a 28-byte entry in a metadata area; it is not derived from the sector number. Per-block subkeys come from HKDF over the volume key.
- Associated data binds the volume uuid, block number and header generation. Torn writes are handled by out-of-place SFS blocks and an A/B superblock and journal record chosen by generation, which depends on the flush barriers of DDR-1143 section 4.2.
- Rollback of an older valid sector is not detected (DDR-1144 threat table). See the plan's proposed list.
- The ledger seed in P2 sector 1 is plaintext on disk (DDR-1153 section 6). See the plan's proposed list.

Other requirements:
- Zeroize the volume key on shutdown and on every panic path. Keys never reach klog. A future S3 or crash-dump writer must never expose keys [EXTERNAL 17].
- Generate keys and salts only after a hardware entropy source is confirmed; fail closed with a clear message otherwise.
- Enforce a minimum passphrase length, a lockout or delay on repeated failures, and a passphrase change that re-wraps rather than re-encrypts.
- On TPM unseal failure, fall back to the passphrase, then the recovery key. Never hard-fail [EXTERNAL 18].
- Measure the KDF in the boot path under QEMU and VMware; store the parameters in the keyslot.
- Keep two copies of the keyslot header, or a checksum plus a backup. Test 4Kn disks.
- Installer safety: refuse a disk that already has a partition table unless the operator types the device and size; power loss during install leaves the disk untouched or clearly incomplete.

## 5. Model sources for agents [REQUIRED]

- [VERIFIED] Local path: `aether/ollama_bridge` and the live path in `user/agent_base.c`, default endpoint 10.0.2.2:11434 (the QEMU host).
- [VERIFIED] Cloud path: `aether/cloud_bridge` exists as Python code on dev/phase1 (DDR-793). Claude Code found no kernel-side code for it: `kernel/` and `user/` contain only comments saying it is deferred. The module documents a prohibition on shipping activation until the kernel authorization and destination-audit risks are closed.
- [VERIFIED by Claude Code] The only bridge credential in the tree is a test string stored through `SYS_VAULT_PUT`/`GET`, and the vault key derives from `g_owner_seed`, which is 32 literal bytes compiled into the image (DDR-1059). A cloud API key therefore cannot be stored there safely.
- [VERIFIED] DDR-1110 states the machine has no on-device natural-language model.
- R-M1 (v1): a model-source layer with three sources: a cloud API key, local ready-made models (Ollama) chosen by hardware, and a signed slot for the OS's own trained model.
- R-M2 (v1): the cloud API key is stored encrypted under the volume key (per-install key plus the DDR-1144 wrap), never in a plaintext /etc/aether/config and never under `g_owner_seed`.
- R-M3 (v1): all sources pass through CAP_NET, the egress allowlist and the audit record.
- R-M4 (T1): a way to point the agent at an Ollama server reachable from VMware, whose NAT address differs from QEMU's 10.0.2.2.

## 6. T1 acceptance

In VMware on the Legion, for both BIOS and UEFI: install to an encrypted disk succeeds; passphrase and recovery unlock work; display, keyboard and mouse work; the network gets an address by DHCP; the agent answers through a local Ollama server; the UI shell shows agents and the approval queue. In QEMU or VMware with emulated devices: multi-touch contacts and gestures work (using `virtio-multitouch-pci`), and a second display can be added and removed. Report pass or fail per item.

## 7. Notes on the 2026-09-29 version of this document

The earlier version cross-referenced section 5.7 for KDF timing; the correct reference is section 4 above.

## 8. Sources ([EXTERNAL n])
1. DP Alt Mode guide: https://us.ktcplay.com/blogs/support-tips/how-to-enable-displayport-alt-mode
2. USB-C DP Alt Mode detection: https://us.ktcplay.com/blogs/support-tips/enable-usb-c-displayport-alt-mode-monitor-not-detected
3. Intel HD Audio specification: https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf
4. Intel I225-V: https://www.intel.com/content/www/us/en/products/sku/184676/intel-ethernet-controller-i225v/downloads.html and Realtek GBE: https://www.realtek.com/Download/List?cate_id=583&menu_id=297
5. USB-IF UCSI: https://www.usb.org/document-library/usb-type-cr-connector-system-software-interface-ucsi-specification
6. Microsoft, USB Type-C bring-up: https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/oem-tasks-for-bringing-up-a-usb-typec
7. I2C HID guide: https://sheridantech.io/2026/05/29/i-2-c-hid-device/
8. HDMI wired to NVIDIA dGPU: https://forums.developer.nvidia.com/t/nvidia-dgpu-in-hybrid-optimus-laptop-not-powering-down-after-unplugging-external-monitor/318196
9. Hybrid wiring discussion: https://forums.freebsd.org/threads/freebsd-on-legacy-nvidia-hybrid-laptop.101808/
10. Microsoft, touchscreen HID protocol: https://learn.microsoft.com/en-us/windows-hardware/design/component-guidelines/touchscreen-protocol-implementation
11. Touch calibration with dual monitors: https://askubuntu.com/questions/253395/touchscreen-calibration-with-dual-monitors-nvidia-and-xinput
12. Microsoft, display and DPI scaling: https://learn.microsoft.com/en-us/windows-hardware/design/component-guidelines/display
13. Per-monitor scaling in Wayland: https://www.reddit.com/r/wayland/comments/s0ve0a/multiple_monitors_with_different_scalingdpi/
14. ACPI specification concepts: https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/03_ACPI_Concepts/ACPI_Concepts.html
15. AES-XTS overview: https://quantumsequrity.com/blog/aes-xts-disk-encryption
16. AEAD nonce reuse: https://p42.studio/en/insights/the-aead-trap-why-nonce-reuse-destroys-symmetric-security
17. LUKS suspend regression: https://mehdirahmani.fr/en/luks-suspend-linux-6-9-encryption-keys-ram/
18. Ubuntu Discourse, dbx update breaks TPM unlock: https://discourse.ubuntu.com/t/after-uefi-dbx-firmware-update-tpm-auto-unlock-doesnt-work-anymore/62730
