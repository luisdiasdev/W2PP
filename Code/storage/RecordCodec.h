/*
 * Conversion between the legacy in-memory structs and the relational row
 * records. The legacy structs remain the single definition of the layout and
 * are never changed by this module.
 */

#ifndef W2PP_STORAGE_RECORDCODEC_H
#define W2PP_STORAGE_RECORDCODEC_H

#include <cstddef>
#include <vector>

#include "Records.h"

struct STRUCT_ACCOUNTFILE;
struct STRUCT_MOB;
struct STRUCT_MOBEXTRA;
struct STRUCT_AFFECT;

namespace storage
{

// Copy a fixed-size legacy name field up to the first NUL. The stored name is
// guaranteed to be shorter than the field so it can be written back as a
// NUL-terminated byte string.
std::vector<unsigned char> NameBytes(const char *field, std::size_t capacity);

// Uppercase bytes with the same per-byte rule as UpcaseString().
std::vector<unsigned char> NameKey(const std::vector<unsigned char> &name);

// Zero the field and copy at most capacity-1 name bytes.
void WriteName(char *field, std::size_t capacity,
	const std::vector<unsigned char> &name);

// Account scalar fields, cargo and all four character slots.
bool AccountRecordFromFile(const STRUCT_ACCOUNTFILE &file, AccountRecord *out);

// Rebuild a fully zero-initialized STRUCT_ACCOUNTFILE. Credential fields are
// left empty; NumericToken keeps its legacy set/unset sentinel.
void AccountFileFromRecord(const AccountRecord &record, STRUCT_ACCOUNTFILE *out);

bool CharacterRecordFromMob(const STRUCT_MOB &mob, const STRUCT_MOBEXTRA &extra,
	const STRUCT_AFFECT *affects, int slot, CharacterRecord *out);

void MobFromCharacterRecord(const CharacterRecord &record, STRUCT_MOB *mob,
	STRUCT_MOBEXTRA *extra, STRUCT_AFFECT *affects,
	unsigned char *short_skill);

// source -> rows -> zero-filled struct. Drops padding and the legacy EMPTY
// byte arrays while preserving every defined field.
void NormalizeAccountFile(const STRUCT_ACCOUNTFILE &source,
	STRUCT_ACCOUNTFILE *normalized);

// FNV-1a over the normalized record. Stable across runs and platforms.
unsigned long long StructuralChecksum(const STRUCT_ACCOUNTFILE &file);

}

#endif
