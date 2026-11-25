# Spec Delta

## Purpose

Protects account credentials by storing only BCrypt hashes of account passwords and numeric secure tokens, verifying them on every authentication path, and removing plaintext credential exposure from logs, exports and admin traffic.

## ADDED Requirements

### Requirement: Account password hashed at rest

Account passwords SHALL be stored only as BCrypt hashes with a unique per-credential salt. The stored value SHALL NOT be reversible to the original password, and no plaintext or reversible encoding SHALL be written to the database, log files, exports or admin replies.

#### Scenario: New account stores a hash

- **WHEN** an account is created with password `secret`
- **THEN** the stored credential is a BCrypt string with a fresh salt and the value `secret` appears nowhere in the database row

#### Scenario: Password change stores a new hash

- **WHEN** an account password is changed
- **THEN** the stored hash verifies the new password, no longer verifies the old password, and uses a salt different from the previous hash

#### Scenario: Identical passwords differ at rest

- **GIVEN** two accounts created with the same password
- **WHEN** both rows are inspected
- **THEN** the two stored hashes differ

### Requirement: Numeric secure token hashed at rest

The 4–6 digit secondary password carried by the existing secure-token packet SHALL be stored only as a BCrypt hash. The unset state SHALL remain distinguishable from an empty or zero token.

#### Scenario: First token binding

- **GIVEN** an account whose token has never been set
- **WHEN** the client binds numeric token `1234`
- **THEN** a BCrypt hash of `1234` is stored and the secure-token success signal is returned

#### Scenario: Token verification

- **GIVEN** an account whose stored token hash corresponds to `1234`
- **WHEN** the client submits `1234`
- **THEN** verification succeeds without storing or logging the plaintext token

#### Scenario: Wrong token

- **GIVEN** an account whose stored token hash corresponds to `1234`
- **WHEN** the client submits `9999`
- **THEN** verification fails with the existing secure-token failure signal

#### Scenario: Unset token is not empty

- **GIVEN** an account that has never bound a token
- **WHEN** it logs in and the secure-token flow runs
- **THEN** the database distinguishes "unset" from a hash and still allows the first binding

### Requirement: All authentication paths verify hashes

Game login, character-deletion confirmation and NP admin login SHALL verify submitted credentials against the stored hash. No authentication path SHALL compare a submitted credential to a stored plaintext value or to another user-supplied value.

#### Scenario: Game login

- **WHEN** a game server submits a login with a correct password
- **THEN** the account is admitted and, with an incorrect password, the existing password-failure signal is returned

#### Scenario: Character deletion

- **WHEN** a character deletion request supplies the account password
- **THEN** the password is verified against the account hash before the deletion rules run

#### Scenario: Admin login

- **WHEN** the NP admin login supplies the account password
- **THEN** the hash is verified and the existing maximum-character-level check still determines the admin level

#### Scenario: Malformed stored hash

- **GIVEN** an account row whose credential value is not a valid hash
- **WHEN** any authentication path verifies a credential for that account
- **THEN** verification fails, the anomaly is logged without the credential, and DBSrv keeps running

### Requirement: Explicit account state

Account state (`normal`, `blocked`, `deleted`, `disabled`) SHALL be stored in its own column and SHALL NOT be inferred from a password prefix. Login, admin display and state transitions SHALL use the state column, while the NP account information reply SHALL keep its existing 0–3 state values.

#### Scenario: Legacy deleted marker

- **GIVEN** a legacy account whose password field starts with `_`
- **WHEN** the account is imported
- **THEN** its state is `deleted`, login is refused, and the admin reply reports state 2

#### Scenario: Legacy blocked marker

- **GIVEN** a legacy account whose password field starts with `@`
- **WHEN** the account is imported
- **THEN** its state is `blocked` and the admin reply reports state 1

#### Scenario: Legacy disabled marker

- **GIVEN** a legacy account whose password field starts with `#`
- **WHEN** the account is imported
- **THEN** its state is `disabled` and the admin reply reports state 3

#### Scenario: Re-enabling an account

- **WHEN** an operator re-enables a blocked or disabled account
- **THEN** the state column returns to `normal` without requiring the password to be reset

### Requirement: Legacy credential conversion

The legacy import SHALL convert each stored plaintext password and numeric token into a BCrypt hash. Prefix-marked values SHALL become the corresponding state with the remainder hashed as the credential; values that cannot be hashed SHALL be imported as locked with an explicit reason instead of being dropped or stored verbatim.

#### Scenario: Plaintext password imported

- **GIVEN** a legacy account with plaintext password `hunter2`
- **WHEN** the import runs
- **THEN** the account can log in with `hunter2` and the database stores only a hash

#### Scenario: Token imported

- **GIVEN** a legacy account with numeric token `987654`
- **WHEN** the import runs
- **THEN** the token verifies successfully after import and the plaintext token is not stored

#### Scenario: Import is resumable

- **GIVEN** an account already converted by a previous import run
- **WHEN** the import runs again
- **THEN** the existing hash is left unchanged and no double hashing occurs

#### Scenario: Unusable credential

- **GIVEN** a legacy credential value that cannot be interpreted
- **WHEN** the import runs
- **THEN** the account is imported in a locked state with a reason recorded, and the import report names it

### Requirement: Transparent hash upgrade

When a successful verification uses a hash whose cost factor or format is below the current policy, DBSrv SHALL replace it with a freshly salted hash of the submitted credential using the configured cost, without requiring the player to change anything.

#### Scenario: Low-cost hash upgraded on login

- **GIVEN** an account whose hash uses a cost below the configured minimum
- **WHEN** the account logs in successfully
- **THEN** the stored hash is replaced with a new hash at the configured cost and the login still succeeds

#### Scenario: Current hash untouched

- **GIVEN** an account already stored at the configured cost
- **WHEN** it logs in successfully
- **THEN** the stored hash is not rewritten

### Requirement: Credential hashing policy

The BCrypt cost factor SHALL be configurable and SHALL default to 12, and the build SHALL reject a configured cost below 10. Password and token hashing SHALL use the same policy, and hashing work SHALL be confined to DBSrv.

#### Scenario: Default cost

- **WHEN** no cost is configured and an account is created
- **THEN** the stored hash names cost 12

#### Scenario: Rejected weak cost

- **WHEN** the configured cost is below 10
- **THEN** DBSrv refuses to start with a message naming the accepted range

#### Scenario: Game server never hashes

- **WHEN** a password or token is created, changed or verified
- **THEN** only DBSrv performs hashing or verification and TMSrv only forwards the submitted bytes

### Requirement: Failed-verification throttling

DBSrv SHALL count consecutive failed password and token verifications per account in the database and SHALL refuse further verification while a configured lockout window is active. The failure limit and window SHALL be configurable, SHALL default to five failures and fifteen minutes, and SHALL reset after a successful verification.

#### Scenario: Token lockout

- **GIVEN** an account with five consecutive failed token verifications
- **WHEN** a sixth token is submitted before the window expires
- **THEN** verification is refused without hashing and the refusal is logged without the submitted token

#### Scenario: Successful verification resets the counter

- **GIVEN** an account with two failed verifications
- **WHEN** the next verification succeeds
- **THEN** the failure counter returns to zero

#### Scenario: Lockout survives restart

- **GIVEN** an account in lockout
- **WHEN** DBSrv restarts
- **THEN** the lockout remains in effect until its window expires

### Requirement: Credential non-disclosure

Passwords, numeric tokens and their hashes SHALL NOT appear in log output, exports, error messages or admin replies. The NP account information reply SHALL carry an empty password field, and bulk import or update logging SHALL identify accounts without including submitted credentials.

#### Scenario: Admin account view

- **WHEN** an operator requests an account through the NP path
- **THEN** the account information reply carries an empty password field and the correct state value

#### Scenario: Import logging

- **WHEN** the bulk user import or update processes an account
- **THEN** the log line names the account and file but contains no password or token

#### Scenario: Export file

- **WHEN** an account is exported
- **THEN** the exported record contains no password, token or hash

### Requirement: Preserved credential byte semantics

Credential verification SHALL use the exact credential bytes accepted today: at most 15 bytes from new input, terminated at the first NUL, compared case-sensitively, with no trimming or case folding. Maximum-length legacy credentials SHALL remain verifiable.

#### Scenario: Case sensitivity

- **GIVEN** an account created with password `Secret`
- **WHEN** a login submits `secret`
- **THEN** the login is refused

#### Scenario: Fifteen-byte password

- **WHEN** an account is created with a 15-character password
- **THEN** verification succeeds with that exact password and fails with a truncated form

#### Scenario: Legacy maximum-length password

- **GIVEN** a legacy account whose 16-byte password field contains no NUL
- **WHEN** the account logs in with the same 16 bytes
- **THEN** verification succeeds after import
