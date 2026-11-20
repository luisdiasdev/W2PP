# Change notes

## Relationship to `linux-x64-port`

This change is the Linux platform plan. It supersedes
`openspec/changes/linux-x64-port`:

- Both changes add native Linux support, but they assume different ABIs.
  `linux-x86-port` keeps the Win32 x86 layout by building Linux i386 with
  `-malign-double` and 64-bit `time_t`; `linux-x64-port` changes bare `long`
  record fields to fixed-width fields for LP64.
- Applying both changes to the same tree would produce contradictory target
  matrices and duplicate source changes. Do not apply them alongside each other.
- The `linux-x64-port` change is intentionally left in place for review; it is
  not deleted or archived here.

All planning artifacts in this change follow the i386 route described in
`design.md` (D1, D2, D8).
