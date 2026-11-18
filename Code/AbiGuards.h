/*
 * Compile-time ABI guards for the wire header and the on-disk records.
 *
 * The closed-source client and the existing account/character files depend on
 * these exact sizes. Any change to a guarded field width must fail the build
 * instead of silently producing an incompatible binary.
 */

#ifndef _ABIGUARDS_
#define _ABIGUARDS_

#include "Basedef.h"
#include "net/Protocol.h"

// Wire protocol header (Code/net/Protocol.h).
static_assert(sizeof(HEADER) == 12,
	"HEADER must stay 12 bytes for wire compatibility");

// Account record written by DBSrv (Code/DBSrv/CFileDB.cpp).
static_assert(sizeof(STRUCT_ACCOUNTFILE) == 7808,
	"STRUCT_ACCOUNTFILE must stay 7808 bytes for on-disk compatibility");

// Character record and its extension, embedded in the account record.
static_assert(sizeof(STRUCT_MOB) == 816,
	"STRUCT_MOB must stay 816 bytes for on-disk compatibility");
static_assert(sizeof(STRUCT_MOBEXTRA) == 536,
	"STRUCT_MOBEXTRA must stay 536 bytes for on-disk compatibility");

// Capsule record written by DBSrv.
static_assert(sizeof(STRUCT_CAPSULE) == 1352,
	"STRUCT_CAPSULE must stay 1352 bytes for on-disk compatibility");

// Selected-character block exchanged with peers.
static_assert(sizeof(STRUCT_SELCHAR) == 840,
	"STRUCT_SELCHAR must stay 840 bytes for wire compatibility");

#endif
