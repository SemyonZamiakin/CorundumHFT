# Security policy

## Supported versions

Only the latest release of Corundum HFT receives security fixes.

## Reporting a vulnerability

Please do not publish exploit details in a public issue.

1. Use GitHub private vulnerability reporting: the **Security** tab of this repository, then **Report a vulnerability**.
2. If that option is not available, open an issue titled "Security contact request" without technical details, and a private channel will be arranged.

Expect an acknowledgement within a few days. This is a small project maintained by one person; there is no bug bounty.

## Threat model

Corundum connects two local processes through a shared-memory segment. What it protects and what it does not:

| Concern | Status |
|---|---|
| A buggy or hostile **writer** making the **reader** read or write outside the mapping | Protected: the reader validates `head` and every record header (read once, then checked) and returns `CRD_ECORRUPT` |
| A hostile writer changing **payload bytes** while the reader uses them | Not protected: shared memory stays writable. Copy the payload, then validate the copy |
| A hostile **reader** corrupting the writer | The writer only reads `tail` and checks it against its own state; a bogus `tail` can make the writer overwrite unread data or report `CRD_FULL`, but not access memory outside the mapping |
| Unrelated users attaching to a ring | Controlled by the segment permissions passed to `crd_create` (`mode`, for example `0600`). `/dev/shm` follows normal file permissions |
| Unrelated users attaching to a ring (Windows) | The mapping lives in the `Local\` namespace of the current logon session and gets the default security of the creating process; `mode` is ignored. Processes of the same user in the same session can attach |
| Name squatting: another user creating the name first | `crd_create` uses `O_EXCL` and fails with `CRD_EEXIST`; `crd_writer_open` / `crd_reader_open` attach to whatever segment has the name. Use names that include your service and user, and check the owner if this matters |
| Denial of service by a peer that stops reading | By design the writer never blocks: it gets `CRD_FULL` and decides what to drop |

## In scope

Memory-safety issues (out-of-bounds access, integer overflow) reachable through the public API or through a peer that writes arbitrary bytes into the segment.

## Notes

- The library performs no heap allocation. Setup functions validate names, capacities and flags.
- CI runs the tests under AddressSanitizer and UndefinedBehaviorSanitizer, on x86-64 and arm64.
