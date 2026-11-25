#include "Database.h"

#include <libpq-fe.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace storage
{
namespace
{

std::string ConnectionErrorMessage(pg_conn *connection)
{
	const char *message = connection != nullptr ? PQerrorMessage(connection) : "";
	if (message == nullptr)
		return std::string();

	std::string text(message);
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
		text.pop_back();
	return text;
}

std::uint32_t ReadUint32(const char *data)
{
	const unsigned char *p = reinterpret_cast<const unsigned char *>(data);
	return (static_cast<std::uint32_t>(p[0]) << 24)
		| (static_cast<std::uint32_t>(p[1]) << 16)
		| (static_cast<std::uint32_t>(p[2]) << 8)
		| static_cast<std::uint32_t>(p[3]);
}

std::uint64_t ReadUint64(const char *data)
{
	const unsigned char *p = reinterpret_cast<const unsigned char *>(data);
	std::uint64_t value = 0;

	for (int i = 0; i < 8; ++i)
		value = (value << 8) | p[i];

	return value;
}

} // namespace

Param ParamText(const std::string &value)
{
	Param param;
	param.storage.assign(value.begin(), value.end());
	// Text-format parameters must be NUL-terminated; PQexecParams ignores
	// paramLengths for format 0.
	param.storage.push_back(0);
	param.length = static_cast<int>(value.size());
	param.format = ParamFormat::Text;
	param.oid = 0;
	return param;
}

Param ParamText(const char *value)
{
	return ParamText(std::string(value != nullptr ? value : ""));
}

Param ParamInt(int value)
{
	char buffer[16];
	std::snprintf(buffer, sizeof(buffer), "%d", value);
	return ParamText(std::string(buffer));
}

Param ParamInt64(long long value)
{
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%lld", value);
	return ParamText(std::string(buffer));
}

Param ParamBytes(const std::vector<unsigned char> &value)
{
	Param param;
	param.storage = value;
	if (param.storage.empty())
		param.storage.push_back(0); // keep data() non-null for an empty bytea
	param.length = static_cast<int>(value.size());
	param.format = ParamFormat::Binary;
	param.oid = OidBytea;
	return param;
}

Param ParamNull()
{
	Param param;
	param.is_null = true;
	param.format = ParamFormat::Text;
	param.oid = 0;
	return param;
}

// ---------------------------------------------------------------------------
// Result
// ---------------------------------------------------------------------------

Result::Result() : result_(nullptr)
{
}

Result::Result(pg_result *result) : result_(result)
{
}

Result::~Result()
{
	if (result_ != nullptr)
		PQclear(result_);
}

Result::Result(Result &&other) noexcept : result_(other.result_)
{
	other.result_ = nullptr;
}

Result &Result::operator=(Result &&other) noexcept
{
	if (this != &other)
	{
		if (result_ != nullptr)
			PQclear(result_);
		result_ = other.result_;
		other.result_ = nullptr;
	}
	return *this;
}

bool Result::Ok() const
{
	if (result_ == nullptr)
		return false;

	ExecStatusType status = PQresultStatus(result_);
	return status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK;
}

bool Result::HasRows() const
{
	return result_ != nullptr && PQresultStatus(result_) == PGRES_TUPLES_OK;
}

int Result::Rows() const
{
	return result_ != nullptr ? PQntuples(result_) : 0;
}

int Result::Columns() const
{
	return result_ != nullptr ? PQnfields(result_) : 0;
}

bool Result::IsNull(int row, int column) const
{
	return result_ == nullptr || PQgetisnull(result_, row, column) != 0;
}

std::string Result::Text(int row, int column) const
{
	if (result_ == nullptr || IsNull(row, column))
		return std::string();

	return std::string(PQgetvalue(result_, row, column),
		static_cast<std::size_t>(PQgetlength(result_, row, column)));
}

std::vector<unsigned char> Result::Bytes(int row, int column) const
{
	std::vector<unsigned char> bytes;

	if (result_ == nullptr || IsNull(row, column))
		return bytes;

	const char *data = PQgetvalue(result_, row, column);
	const int length = PQgetlength(result_, row, column);

	bytes.assign(data, data + length);
	return bytes;
}

int Result::Int16(int row, int column) const
{
	if (result_ == nullptr || IsNull(row, column))
		return 0;

	if (PQfformat(result_, column) == 0)
		return std::atoi(PQgetvalue(result_, row, column));

	if (PQgetlength(result_, row, column) < 2)
		return 0;

	const unsigned char *p =
		reinterpret_cast<const unsigned char *>(PQgetvalue(result_, row, column));
	return static_cast<std::int16_t>((static_cast<unsigned int>(p[0]) << 8)
		| static_cast<unsigned int>(p[1]));
}

int Result::Int32(int row, int column) const
{
	if (result_ == nullptr || IsNull(row, column))
		return 0;

	if (PQfformat(result_, column) == 0)
		return std::atoi(PQgetvalue(result_, row, column));

	if (PQgetlength(result_, row, column) < 4)
		return 0;

	return static_cast<int>(ReadUint32(PQgetvalue(result_, row, column)));
}

long long Result::Int64(int row, int column) const
{
	if (result_ == nullptr || IsNull(row, column))
		return 0;

	if (PQfformat(result_, column) == 0)
		return std::atoll(PQgetvalue(result_, row, column));

	if (PQgetlength(result_, row, column) < 8)
		return 0;

	return static_cast<long long>(ReadUint64(PQgetvalue(result_, row, column)));
}

bool Result::Bool(int row, int column) const
{
	if (result_ == nullptr || IsNull(row, column))
		return false;

	if (PQfformat(result_, column) == 0)
	{
		const char *value = PQgetvalue(result_, row, column);
		return value[0] == 't' || value[0] == 'T' || value[0] == '1';
	}

	return PQgetlength(result_, row, column) > 0
		&& PQgetvalue(result_, row, column)[0] != 0;
}

std::string Result::Error() const
{
	if (result_ == nullptr)
		return std::string();

	const char *message = PQresultErrorMessage(result_);
	if (message == nullptr)
		return std::string();

	std::string text(message);
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
		text.pop_back();
	return text;
}

std::string Result::SqlState() const
{
	if (result_ == nullptr)
		return std::string();

	const char *state = PQresultErrorField(result_, PG_DIAG_SQLSTATE);
	return state != nullptr ? std::string(state) : std::string();
}

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------

Database::Database() : connection_(nullptr)
{
}

Database::~Database()
{
	Close();
}

void Database::Close()
{
	if (connection_ != nullptr)
	{
		PQfinish(connection_);
		connection_ = nullptr;
	}
}

bool Database::IsOpen() const
{
	return connection_ != nullptr && PQstatus(connection_) == CONNECTION_OK;
}

bool Database::Connect(const StorageConfig &config, std::string *error)
{
	Close();

	std::vector<const char *> keywords;
	std::vector<const char *> values;

	const std::string portText = std::to_string(config.port);
	const std::string connectTimeoutText = std::to_string(config.connect_timeout_seconds);

	keywords.push_back("host");
	values.push_back(config.host.c_str());
	keywords.push_back("port");
	values.push_back(portText.c_str());
	keywords.push_back("dbname");
	values.push_back(config.database.c_str());
	keywords.push_back("user");
	values.push_back(config.user.c_str());
	keywords.push_back("sslmode");
	values.push_back(config.sslmode.c_str());
	keywords.push_back("connect_timeout");
	values.push_back(connectTimeoutText.c_str());
	keywords.push_back("application_name");
	values.push_back("w2pp_dbsrv");

	if (!config.password.empty())
	{
		keywords.push_back("password");
		values.push_back(config.password.c_str());
	}

	keywords.push_back(nullptr);
	values.push_back(nullptr);

	connection_ = PQconnectdbParams(keywords.data(), values.data(), 0);

	if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK)
	{
		std::string detail = ConnectionErrorMessage(connection_);
		if (error != nullptr)
			*error = "cannot connect to PostgreSQL at " + config.Endpoint()
				+ (detail.empty() ? std::string() : ": " + detail);
		Close();
		return false;
	}

	// Silence NOTICE output; warnings and errors still surface through results.
	PQsetNoticeProcessor(connection_, [](void *, const char *) {}, nullptr);

	if (!SetTimeouts(config, error))
	{
		Close();
		return false;
	}

	return true;
}

bool Database::SetTimeouts(const StorageConfig &config, std::string *error)
{
	Result result = Exec("SET statement_timeout = " + std::to_string(config.statement_timeout_ms)
		+ "; SET lock_timeout = " + std::to_string(config.lock_timeout_ms) + ";");

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "cannot apply statement/lock timeout: " + result.Error();
		return false;
	}

	return true;
}

Result Database::Exec(const std::string &sql)
{
	if (connection_ == nullptr)
		return Result();

	return Result(PQexec(connection_, sql.c_str()));
}

Result Database::ExecParams(const std::string &sql,
	const std::vector<Param> &params, bool binaryResults)
{
	if (connection_ == nullptr)
		return Result();

	std::vector<const char *> values(params.size());
	std::vector<int> lengths(params.size());
	std::vector<int> formats(params.size());
	std::vector<unsigned int> types(params.size());

	for (std::size_t i = 0; i < params.size(); ++i)
	{
		values[i] = static_cast<const char *>(params[i].Data());
		lengths[i] = params[i].Length();
		formats[i] = static_cast<int>(params[i].format);
		types[i] = params[i].oid;
	}

	return Result(PQexecParams(connection_, sql.c_str(),
		static_cast<int>(params.size()),
		types.empty() ? nullptr : types.data(),
		values.empty() ? nullptr : values.data(),
		lengths.empty() ? nullptr : lengths.data(),
		formats.empty() ? nullptr : formats.data(),
		binaryResults ? 1 : 0));
}

bool Database::Prepare(const std::string &name, const std::string &sql,
	std::string *error)
{
	if (connection_ == nullptr)
	{
		if (error != nullptr)
			*error = "database is not connected";
		return false;
	}

	Result result(PQprepare(connection_, name.c_str(), sql.c_str(), 0, nullptr));

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "cannot prepare " + name + ": " + result.Error();
		return false;
	}

	return true;
}

Result Database::ExecPrepared(const std::string &name,
	const std::vector<Param> &params, bool binaryResults)
{
	if (connection_ == nullptr)
		return Result();

	std::vector<const char *> values(params.size());
	std::vector<int> lengths(params.size());
	std::vector<int> formats(params.size());

	for (std::size_t i = 0; i < params.size(); ++i)
	{
		values[i] = static_cast<const char *>(params[i].Data());
		lengths[i] = params[i].Length();
		formats[i] = static_cast<int>(params[i].format);
	}

	return Result(PQexecPrepared(connection_, name.c_str(),
		static_cast<int>(params.size()),
		values.empty() ? nullptr : values.data(),
		lengths.empty() ? nullptr : lengths.data(),
		formats.empty() ? nullptr : formats.data(),
		binaryResults ? 1 : 0));
}

bool Database::Begin(const char *isolation, std::string *error)
{
	std::string sql = "BEGIN";

	if (isolation != nullptr && isolation[0] != '\0')
		sql += " ISOLATION LEVEL ";
	if (isolation != nullptr && isolation[0] != '\0')
		sql += isolation;

	Result result = Exec(sql);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "BEGIN failed: " + result.Error();
		return false;
	}

	return true;
}

bool Database::Commit(std::string *error)
{
	Result result = Exec("COMMIT");

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "COMMIT failed: " + result.Error();
		return false;
	}

	return true;
}

bool Database::Rollback()
{
	Result result = Exec("ROLLBACK");
	return result.Ok() || connection_ == nullptr;
}

std::string Database::LastError() const
{
	return ConnectionErrorMessage(connection_);
}

pg_conn *Database::Raw() const
{
	return connection_;
}

// ---------------------------------------------------------------------------
// Transaction
// ---------------------------------------------------------------------------

Transaction::Transaction(Database &database)
	: database_(database), active_(false)
{
}

Transaction::~Transaction()
{
	if (active_)
		database_.Rollback();
}

bool Transaction::Begin(std::string *error, const char *isolation)
{
	if (active_)
	{
		if (error != nullptr)
			*error = "transaction already active";
		return false;
	}

	if (!database_.Begin(isolation, error))
		return false;

	active_ = true;
	return true;
}

bool Transaction::Commit(std::string *error)
{
	if (!active_)
	{
		if (error != nullptr)
			*error = "no active transaction";
		return false;
	}

	active_ = false;
	return database_.Commit(error);
}

void Transaction::Rollback()
{
	if (active_)
	{
		database_.Rollback();
		active_ = false;
	}
}

} // namespace storage
