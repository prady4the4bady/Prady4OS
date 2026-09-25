# DDR-1141 — DHCP and DNS, and the egress they open

**Status:** design, committed before code (§NON-NEGOTIABLE 5).
**Operator instruction:** PR #17 comment 5822830053, item 4 (OWNER-verified):
*"DHCP/DNS enabled and the hardcoded 10.0.2.15/24 default removed. Needs its own
DDR and gate. The allowlist and audit records must cover the new egress path;
do not skip that coverage."*

## 1. What is there today, measured

- `lwipopts.h` has `LWIP_DHCP 0` (*"static IP (ADR-025 §D5)"*) and
  `LWIP_DNS 0`.
- `net_init()` (`lwip_port.c`) hardcodes `10.0.2.15/24` with gateway
  `10.0.2.2`. These are QEMU slirp's defaults. On any other network the
  interface is simply wrong, and nothing says so.
- Ring 3 cannot name a host at all. `SYS_SOCK_CONNECT` takes an IPv4 value
  (`host_be`, first octet in the most significant byte).
- The egress controls are privacy mode (DDR-802/1070), the CAP_NET flag
  (DDR-731), the allowlist (DDR-734) and the audit records for sovereign bypass
  and allowed connects (DDR-800/801). **All of them live in `sys_sock_connect`
  and the socket I/O paths.** A DNS query is new egress, and **none of those
  controls would see it unless it is routed through them.** That is the
  DDR-1070 shape, where a control that exists elsewhere does not cover the new
  path. The operator named this risk explicitly.

## 2. DHCP

### 2.1 Behaviour

- `netif_add` with `0.0.0.0`, then `dhcp_start(&g_netif)`.
- `net_init` waits **at most 5 s** (`g_ticks` deadline) for a lease. It pumps
  lwIP between timer interrupts; interrupts are already enabled when `net_init`
  runs (`main.c` enables them before calling it).
- On a lease it prints one line:
  `[net] lwIP up <ip>/<prefix> gw=<gw> dns=<dns> via dhcp`.
  - Every field comes from the lease. None is a literal.
  - On QEMU's default slirp network the lease *is* 10.0.2.15/24, so the
    existing `smoke-net-lo` sentinel `[net] lwIP up 10.0.2.15/24` still
    matches. That substring is now data, not a constant.
- With no lease within the deadline it prints
  `[net] dhcp: no lease within 5 s -- interface unconfigured` and boot
  continues. The loopback interface and every loopback self-test are
  unaffected. lwIP keeps retrying in the background, and a later lease prints
  the same `lwIP up` line from the netif status callback.
- **There is no static fallback.** That is the operator's instruction. A wrong
  address that looks configured is worse than an unconfigured interface that
  says so.
- **Address conflict detection (`LWIP_ACD`) is off.** It would add seconds of
  probing to every boot, and slirp cannot conflict. This is stated as a
  limitation for real LANs.

### 2.2 The SYN-fuzz self-test addresses the lease, not a constant

`build_syn` hardcoded the destination `10.0.2.15`. On any other lease, every
flood segment would be dropped at the IP layer before TCP parsing, and the fuzz
arm would quietly test less. That is the DDR-1070 class again. It now uses
`netif_ip4_addr(&g_netif)`. If there is no lease the flood is skipped, and the
log says so.

### 2.3 DHCP egress is kernel-originated and is audited, not allowlisted

- DHCP traffic is broadcast and server-directed configuration. No process asks
  for it. The allowlist governs **destinations a caller chooses**, so it does
  not apply here, and applying it would be circular: no lease, no network.
- **It is audited.** When the first lease is bound, `net_init` (thread
  context, not the lwIP callback) writes one record. **Corrected when built:**
  the record is written from `kmain` immediately after `aether_sectest()`,
  because the audit ring does not exist yet when `net_init` returns. A lease
  that arrives **after** the 5 s wait is printed by the status callback but is
  **not audited**. Stated, not hidden. The record: `pid 0`,
  `ACTION_NET_DHCP` (new, appended, value 15), id = `AETHER_DEST_ID(server,
  67)`, result `AR_NET_CONNECT`.
  - The record states that this machine took network configuration from that
    server.
  - Renewals are not separately audited. lwIP renews at half-lease, which is
    12 h on slirp.
- **Privacy mode does not stop DHCP renewal.** Dropping the lease would take
  the interface down, which is a different control from "nothing leaves on an
  agent's behalf". Privacy mode governs caller-directed egress. This is stated
  in the release notes as a v1 property, not left implicit.

## 3. DNS: `SYS_DNS_RESOLVE` (NSI 103)

```
SYS_DNS_RESOLVE(const char *name, uint32_t *out_host_be, uint32_t resolver_be)
  -> 0 | -EPERM | -EFAULT | -ENAMETOOLONG | -EINVAL | -ENOENT | -ETIMEDOUT | -EBUSY
```

> **CORRECTED AT THE SITE, before shipping (DDR-1110's rule).** Three
> details of the design above changed when it was built. The text is left as
> designed, so the record shows what was believed first.
> - **`errno.h` has no `EBUSY`.** A second caller while a query is in flight
>   gets **`-EAGAIN`**.
> - **No lease (so no resolver), or no network, returns `-ENODEV`, not
>   `-ENOENT`.** `-ENOENT` is kept for "the name did not resolve", so a caller
>   can tell "no network" from "no such name".
> - **The wait is `sti; hlt; cli` against a `g_ticks` deadline, not `yield()`.**
>   It is the same wait `sys_sock_read` uses. `hlt` wakes on the next RX or
>   timer interrupt, and the wait is bounded by the deadline, not by a spin
>   count.

- `resolver_be == 0` means the resolver learned from DHCP.
  - If there is none, the call returns `-ENOENT`.
- A non-zero `resolver_be` names an explicit resolver. It exists for two
  reasons:
  1. **Policy.** It is a real destination, and it goes through the same
     allowlist check as a connect.
  2. **Testing.** QEMU slirp refuses a DHCP DNS address equal to its host alias
     (*"DNS must be different from host"*, measured). Every DHCP-learned
     resolver under slirp is therefore slirp's proxy, which forwards to the host
     machine's real resolvers, i.e. the internet. A strict gate cannot depend on
     that.

**The checks, in `sys_sock_connect`'s order and for its reasons:**

1. **Privacy mode.** First, and ahead of the sovereign bypass. A query that is
   refused must **not be sent**, because the query itself leaks the name.
   Audited `AR_PRIVACY_BLOCKED`.
2. **CAP_NET** (`is_net`, or sovereign). Audited `AR_CAP_DENIED`.
3. **Allowlist** on `(resolver, 53)` for non-sovereign callers. Audited
   `AR_CAP_DENIED`.
4. **Sovereign bypass** recorded as `AR_SOVEREIGN_BYPASS`; otherwise
   `AR_NET_CONNECT`. This record is written **before** the query goes out, per
   DDR-801's rule that an authorised attempt is recorded independently of
   network conditions.

Every record is `ACTION_NET_DNS` (new, appended, value 14), with id =
`AETHER_DEST_ID(resolver, 53)`. The **queried name is not recorded**: records
carry integers only. That is stated, not glossed.

**Mechanism:**
- One query is in flight at a time. A second caller gets `-EBUSY`.
- The resolver is set into lwIP's server slot 0 under `g_net_lock` for that
  query.
- The caller waits with `yield()` against a 5 s `g_ticks` deadline.
  `yield()` opens an interrupt window (DDR-981), so RX and timers run.
- The lwIP callback carries a **generation number**, so a late answer to an
  abandoned query is ignored rather than written into the next caller's result.

**Stated limitation:** lwIP's DNS cache is keyed by name, not by resolver, so a
name cached from one resolver can be answered from cache when another is asked.
The audit record is written either way.

## 4. The gate: `smoke-dhcpdns`

It uses one boot, on a **non-default** slirp network
(`net=10.77.0.0/24,host=10.77.0.2,dhcpstart=10.77.0.40,dns=10.77.0.3`),
through a new `boot_test.sh` hook, `QEMU_NET_USER_OPTS`. A host-side responder
(`tools/ci/dns_responder.py`) binds `127.0.0.1:53`, which slirp's host alias
`10.77.0.2:53` reaches. Binding port 53 needs root; the gate uses `sudo -n` when
not already root, which is DDR-1104 §3's recorded cost, accepted here. The
responder answers `*.pradyos.test` with `10.77.0.99` and **logs every name it
receives**.

**Arms:**

| Arm | What | Why it discriminates |
|---|---|---|
| D | `[net] lwIP up 10.77.0.40/24 gw=10.77.0.2 dns=10.77.0.3 via dhcp` | A hardcoded 10.0.2.15 kernel cannot print it, and a client that ignores the options prints something else. |
| A | a CAP_NET, non-sovereign probe, with the allowlist seeded to `10.77.0.2:53`, resolves `allowed.pradyos.test` → `10.77.0.99` | That value can only come from the host responder, so it proves a real query and a real answer over the NIC. |
| L | same probe, explicit resolver `10.77.0.5` (not allowlisted) → exactly `-EPERM` | |
| P | a sovereign instance with privacy on resolves `private.pradyos.test` → exactly `-EPERM`; then privacy off and `released.pradyos.test` → `10.77.0.99` | The release half shows the refusal was privacy, not a broken path. |
| U | the non-sovereign probe reads the audit ring and finds `ACTION_NET_DNS` records with `AR_NET_CONNECT` and `AR_CAP_DENIED` for its own pid | |
| H | the host responder's log contains `allowed` and `released` and **does not** contain `denied` or `private` | Catches a kernel that audits a refusal but sends the packet anyway. **This is the arm the operator's "do not skip that coverage" needs.** |

**Mutants, each on a recorded hash:**
- **M1:** no allowlist check in the DNS path. Fails L, and H: `denied` reaches
  the host.
- **M2:** no privacy check. Fails P, and H: `private` reaches the host.
- **M3:** no audit record. Fails U.
- **M4:** the literal `10.0.2.15` restored instead of DHCP. Fails D.

## 5. Not claimed

- **No IPv6** (DDR-1104 §2 is unchanged).
- **No mDNS, no DNSSEC, no resolver cache policy.**
- Real-LAN DHCP servers are **untested**; only slirp's is exercised. The client
  is lwIP's own implementation.
- Address conflict detection is off (§2.1).
- Privacy mode does not stop DHCP renewal (§2.3).
- The queried name is not in the audit record.

## 6. Results, measured

**Status:** built and gated. `smoke-dhcpdns` is registered on a shard (181 gates),
`kernel.bin` is **1,352,074 B** (220,790 B headroom), warning-clean at `-Werror`,
and hygiene reports ALL NINE PASSED.

**Clean kernel `d8d9492f3bb17e6a`: rc=0, twice** (once before and once after the
§6.1 gate change). The responder log reads `allowed.pradyos.test`,
`released.pradyos.test` and nothing else.

| Mutant | kernel | Guest arm that failed | Arm H | Observed line |
|---|---|---|---|---|
| M1 allowlist check off | `7b789302e19f63ca` | **L** | **FAIL — `denied.pradyos.test` reached the resolver** | `PRADYOS_DNS_L rc=0 ip=0x0A4D0063` |
| M2 privacy check off | `0d527401bc76f8ca` | **P** | **FAIL — `private.pradyos.test` reached the resolver** | `PRADYOS_DNS_P mode=00 on=0 onip=0x0A4D0063 …` |
| M3 audit record off | `fe23e8e16bef10fc` | **U2** | n/a (no leak) | `PRADYOS_DNS_U2 allowed=0` |
| M4 DHCP not started, 10.0.2.15 literal | `bb3ef0cba55d069a` | **D** | n/a | `[net] dhcp: no lease within 5 s -- interface unconfigured` |

Each mutant fails a **different** guest arm, so none is covered only because
another arm happened to catch it. The revert rebuilds to `d8d9492f3bb17e6a`
**bit-for-bit**, checked by rebuilding rather than assumed.

**Where §4's predictions did not match what was measured:**
- **M3 fails U2, not "U".** U1 (`denied=`) is written on the refusal path, so
  U2 (`allowed=`) is the arm that sees a missing record on the authorised path.
- **M4 prints a DHCP timeout, not the literal address.** Once `dhcp_start` is
  skipped, the probe's queries fail with `-ETIMEDOUT`/`-EPERM` and never report
  a wrong address. Arm D catches it either way.

### 6.1 A defect in my own gate, found by the mutants: arm H could not fail on M1 or M2

The first mutant campaign put M1 on L and M2 on P, which is correct. **It did not
show the leak itself.** `dhcpdns_gate.sh` returned `boot_test.sh`'s rc
*before* reading the responder log. A kernel that leaks a refused name almost
always also misprints a guest sentinel, as M1 and M2 do, so the one arm that
watches the far end **could only run on kernels that did not leak**. That is the
dead-arm class. It is worse here because §4 calls arm H *"the arm the operator's
'do not skip that coverage' needs."*

**Fix:** arm H now runs on every run. The script exits with the guest rc if
that is non-zero, and otherwise with arm H's verdict. **Re-measured on the same
mutant hashes:** M1's responder log contains `denied.pradyos.test` and M2's
contains `private.pradyos.test`, each named by `FAIL arm H`. So the claim that a
refused query is **not sent** is now shown on the wire, not inferred from a
return code.

### 6.2 Not claimed (additional to §5)

- No rate is claimed. Each row is one boot on a pinned hash.
- **A real LAN is untested.** On slirp, `10.77.0.3` is slirp's DNS proxy, which
  the gate deliberately bypasses by naming the host alias explicitly (§3).
- No open issue moves (OPEN-1/2/12/13 untouched). GLOBAL_FORBIDDEN is unchanged
  at 77: the gate's refusal arms are exact required patterns, and the check is
  deterministic.
