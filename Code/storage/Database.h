/*
 * Thin RAII wrapper over libpq.
 *
 * The storage layer deliberately stays on the error-code style the legacy
 * server uses: operations return bool/Result and never throw. All queries use
 * bound parameters; schema identifiers are compile-time constants.
 */

#ifndef W2PP_STORAGE_DATABASE_H
#define W2PP_STORAGE_DATABASE_H

#include <memory>
#include <string>
#include <vector>

#include "StorageConfig.h"

struct pg_conn;
struct pg_result;

namespace storage
{

enum class ParamFormat
{
	Text = 0,
	Binary = 1
};

struct Param
{
	// Owned payload. Keeping the bytes in the parameter makes a vector of
	// parameters safe to build in one expression; data/length are derived at
	// call time so copies never leave a dangling pointer.
	std::vector<unsigned char> storage;
	int length = 0;
	ParamFormat format = ParamFormat::Text;
	unsigned int oid = 0; // 0 lets the server infer the type
	bool is_null = false;

	const void *Data() const { return is_null ? nullptr : storage.data(); }
	int Length() const { return length; }
};

Param ParamNull();

Param ParamText(const std::string &value);
Param ParamText(const char *value);
Param ParamInt(int value);
Param ParamInt64(long long value);
Param ParamBytes(const std::vector<unsigned char> &value);

// PostgreSQL type OIDs used by the storage layer.
enum : unsigned int
{
	OidBytea = 17,
	OidInt8 = 20,
	OidInt4 = 23,
	OidInt2 = 21,
	OidText = 25,
	OidBool = 16
};

class Result
{
public:
	Result();
	explicit Result(pg_result *result);
	~Result();

	Result(const Result &) = delete;
	Result &operator=(const Result &) = delete;
	Result(Result &&other) noexcept;
	Result &operator=(Result &&other) noexcept;

	bool Ok() const;          // tuples or command completed
	bool HasRows() const;
	int Rows() const;
	int Columns() const;
	bool IsNull(int row, int column) const;

	std::string Text(int row, int column) const;
	std::vector<unsigned char> Bytes(int row, int column) const;
	int Int16(int row, int column) const;
	int Int32(int row, int column) const;
	long long Int64(int row, int column) const;
	bool Bool(int row, int column) const;

	std::string Error() const;
	std::string SqlState() const;

	pg_result *Raw() const { return result_; }

private:
	pg_result *result_;
};

class Database
{
public:
	Database();
	~Database();

	Database(const Database &) = delete;
	Database &operator=(const Database &) = delete;

	bool Connect(const StorageConfig &config, std::string *error);
	void Close();
	bool IsOpen() const;

	// Unprepared execution (DDL and transaction control only).
	Result Exec(const std::string &sql);

	Result ExecParams(const std::string &sql,
		const std::vector<Param> &params, bool binaryResults);

	bool Prepare(const std::string &name, const std::string &sql,
		std::string *error);
	Result ExecPrepared(const std::string &name,
		const std::vector<Param> &params, bool binaryResults);

	bool Begin(const char *isolation, std::string *error);
	bool Commit(std::string *error);
	bool Rollback();

	bool SetTimeouts(const StorageConfig &config, std::string *error);

	std::string LastError() const;
	pg_conn *Raw() const;

private:
	pg_conn *connection_;
};

/*
 * Rolls back on destruction unless the owner committed. Nested use is not
 * allowed; the repository opens one transaction per operation.
 */
class Transaction
{
public:
	explicit Transaction(Database &database);
	~Transaction();

	Transaction(const Transaction &) = delete;
	Transaction &operator=(const Transaction &) = delete;

	bool Begin(std::string *error, const char *isolation = nullptr);
	bool Commit(std::string *error);
	void Rollback();
	bool Active() const { return active_; }

private:
	Database &database_;
	bool active_;
};

}

#endif
