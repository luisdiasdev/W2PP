# Storage schema

DBSrv stores player state in PostgreSQL in the `w2pp` schema. This document is
the operator/reviewer reference for the tables, the invariants they enforce and
the migration mechanism. The DDL lives in `Code/storage/Schema.cpp`
(migration version 1); every table and constraint named here exists in that
file.

## Migration and metadata

| Table | Purpose |
| --- | --- |
| `schema_migration` | One row per applied migration (`version`, `applied_at`). The highest version is the schema version the binary expects. |
| `store_metadata` | Key/value store. Holds `schema_contract` (the layout contract the binary understands) and `import_state` (`pending`, `complete`, `failed`). |

The runner takes `pg_advisory_lock(0x77327070)` so concurrent DBSrv or importer
processes cannot migrate in parallel. It then creates the `w2pp` schema and the
two bootstrap tables, compares `store_metadata.schema_contract` with the
binary's contract (refusing a newer database), and applies each pending step in
its own `SERIALIZABLE` transaction. A step records its version only after its
DDL committed; on failure the transaction rolls back, the previous version
stays recorded and the error names the failing step.

## Entities

### `accounts`

One row per account. Scalar account fields, the 64-byte quick-skill block and
the 128-slot account cargo (separate table) live here.

- `account_id` — generated identity.
- `name` / `name_key` — raw legacy bytes and their uppercase identity. `name_key`
  is unique (`accounts_name_key_unique`). Names are `bytea`, not `text`, because
  legacy names may not be valid UTF-8; equality remains byte-exact.
- `password_hash` / `token_hash` — BCrypt strings. `token_hash IS NULL` is the
  unset token state. No plaintext or reversible value is stored.
- `state` — 0 normal, 1 blocked, 2 deleted, 3 disabled
  (`accounts_state_range`). Replaces the legacy `@`/`_`/`#` password prefix.
- `block_year` / `block_year_day` — legacy dated block fields.
- `temp_key` — the 52-byte server-change token, exactly 52 bytes
  (`accounts_temp_key_width`).
- `short_skill` — 64 bytes, exactly (`accounts_short_skill_width`).
- `coin` — signed; legacy negative values are clamped to zero during login
  admission rather than rejected at import.
- `password_failures`/`password_locked_until` and
  `token_failures`/`token_locked_until` — throttling state, durable across
  restarts.
- `created_at` / `updated_at` — `updated_at` replaces the legacy file mtime for
  the day-log and ranking scans (`accounts_updated_at_idx`).

Width checks: `name` and `name_key` ≤ 16 bytes (`accounts_name_width`,
`accounts_name_key_width`), `real_name` ≤ 24 (`accounts_real_name_width`),
`email` ≤ 48 (`accounts_email_width`), `telephone` ≤ 16
(`accounts_telephone_width`), `address` ≤ 78 (`accounts_address_width`).

### `characters`

One row per live character **and** per sealed soul capsule. Equipment,
inventory, affects, extras and celestial saves reference `character_id`, so
sealing and restoring move one row instead of copying child data.

- `character_id` — generated identity.
- `account_id` + `slot` — set only for live characters; `slot` is 0–3
  (`characters_slot_range`).
- `capsule_id` — set only for sealed rows; 1–65535
  (`characters_capsule_range`) because the identifier is encoded in two item
  effect bytes.
- `characters_owner_exclusive` — exactly one of the live/capsule forms.
- `name` / `name_key` — upper-cased unique name (`characters_name_key_key`).
  The global uniqueness reproduces the legacy `./char` marker lifetime: sealing
  keeps the old name reserved until the capsule is taken out under a new name.
- `characters_live_slot_key` — unique `(account_id, slot)` for live rows only.
- `characters_capsule_id_key` — unique `capsule_id` for sealed rows.
- `characters_account_id_idx` — supports loading all characters of an account.
- `characters_name_width`, `characters_name_key_width` — ≤ 16 bytes;
  `characters_skill_bar_width` — exactly 4 bytes.
- Remaining columns are the `STRUCT_MOB` scalars: clan, merchant, guild, class,
  rsv, quest, coin, exp, spx/spy, learned_skill, magic, bonuses, critical,
  save_mana, skill_bar, guild_level, regen, resist0–3, plus the ten `STRUCT_SCORE`
  values for both `base_*` and `current_*` (level, ac, damage, merchant,
  attack_run, direction, chaos_rate, max_hp/mp, hp/mp, str/int/dex/con, four
  specials).

### Child item and affect arrays

| Table | Key | Range | Contents |
| --- | --- | --- | --- |
| `character_equipment` | `(character_id, position)` | 0–15 (`character_equipment_position_range`) | `item_index`, `effect0..2` |
| `character_inventory` | `(character_id, position)` | 0–63 (`character_inventory_position_range`) | `item_index`, `effect0..2` |
| `account_cargo` | `(account_id, position)` | 0–127 (`account_cargo_position_range`) | `item_index`, `effect0..2` |
| `character_affects` | `(character_id, position)` | 0–31 (`character_affects_position_range`) | `type`, `value`, `level`, `time` |

Only non-empty positions get a row. An empty item position is reconstructed as
`item_index = 0` with zero effects; an empty affect is `type = 0`. The primary
key keeps positions explicit, so sparse arrays (including cargo at 0 and 127
only) round-trip without shifting. Duplicate item indices at different
positions are independent rows. Affect ranges: type 0–255
(`character_affects_type_range`), value −128–255
(`character_affects_value_range`), level 0–65535
(`character_affects_level_range`), time 0–4294967295
(`character_affects_time_range`).

### `character_extras`

One row per character holding `STRUCT_MOBEXTRA`: class master, citizen, fame,
soul, mortal face, the mortal/arch/celestial quest fields, circle, last NT/NT,
kefra ticket, divine end, hold, day-log exp/year-day and donate
last-time/count. The legacy `EMPTY` arrays are deliberately not stored.

### `character_celestial_saves`

`(character_id, save_slot)` with `save_slot` 0–1
(`character_celestial_slots`). Each row holds one `SaveCelestial` block:
class, exp, spx/spy, the ten base score values, learned skill, bonuses,
`skill_bar1` (exactly 4 bytes, `character_celestial_bar1_width`) and
`skill_bar2` (exactly 16 bytes, `character_celestial_bar2_width`), and soul.

### `credential_events`

Append-only audit of failed or locked verifications:
`account_id`, `credential_kind` (1 password, 2 token,
`credential_events_kind`), `occurred_at`, `source`
(`credential_events_account_idx`). It never contains credential material.

## Record reconstruction invariants

- All child tables cascade on character/account deletion.
- `STRUCT_ACCOUNTFILE`, `STRUCT_MOB`, `STRUCT_MOBEXTRA` and `STRUCT_CAPSULE`
  keep their guarded sizes and offsets (`Code/AbiGuards.h`).
- Reading rebuilds a zero-filled struct from rows; missing item/affect positions
  become zero entries and legacy `EMPTY`/padding bytes stay zero.
- Account and character names are stored as the bytes before the first NUL and
  written back NUL-terminated, so an imported record can be re-encoded without
  reading past a field.
- `AccountPass` is empty on every read; `NumericToken[0]` is `-1` only when
  `token_hash IS NULL`.
- The importer's structural checksum is FNV-1a over the normalized 7808-byte
  record (`storage::StructuralChecksum`), so it is stable across runs.

## Operational notes

- DBSrv needs DML privileges. DDL privileges are needed at first start or when
  a migration is pending; the design allows a separate migration role.
- `import_state` must be `complete` before DBSrv accepts traffic. The importer
  sets `failed` and exits non-zero when records were rejected.
- Name columns are `bytea`; use `convert_from(name, 'EUC_KR')` in ad-hoc SQL
  only where the bytes are known-valid CP949.
