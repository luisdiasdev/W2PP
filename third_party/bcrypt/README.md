# Vendored BCrypt

This directory contains the password-hashing implementation used by DBSrv.
Hashing is only ever performed in DBSrv; TMSrv forwards credential bytes
unchanged.

## Origin

`bcrypt.c` and `blowfish.c` are the OpenBSD libc crypt implementation:

- <https://github.com/openbsd/src/blob/master/lib/libc/crypt/bcrypt.c>
- <https://github.com/openbsd/src/blob/master/lib/libc/crypt/blowfish.c>

Both files keep their original copyright notices. `LICENSE` reproduces both
licenses; both are permissive and compatible with the GPLv3 project.

## Changes for this build

The algorithm is unchanged. Only the platform plumbing was adapted so the
files compile as C11 with MSVC (Win32) and Linux i386 gcc:

- `blf.h` is a portable replacement for the OpenBSD header. It takes fixed
  width types from `<stdint.h>`, provides the BSD `u_intN_t` spellings and
  defines `DEF_WEAK` as a no-op off OpenBSD.
- `bcrypt.c` includes `blf.h` locally, drops `<pwd.h>`/`<time.h>` and the
  unused `_bcrypt_autorounds()` helper, and calls the shims below instead of
  BSD-only `arc4random_buf`, `explicit_bzero` and `timingsafe_bcmp`.
- `blowfish.c` includes `blf.h` locally.

## Added files

- `w2pp_bcrypt.c` — `explicit_bzero`/`timingsafe_bcmp` shims plus the two
  exported functions declared in `w2pp_bcrypt.h`.
- `w2pp_bcrypt_random.cpp` — salt bytes from `std::random_device` (OS CSPRNG).
- `blf.h`, `w2pp_bcrypt.h`, `LICENSE`, this README.

New hashes use the `$2b$` prefix; verification accepts `$2a$` and `$2b$`.
