/*	-------------------------------------------------------------------------------------------------------
	SqliteLogger implementation. See SqliteLogger.h for documentation and usage examples.
	------------------------------------------------------------------------------------------------------- */

#include "CvGameCoreDLLPCH.h"
#include "SqliteLogger.h"
#include "CustomMods.h"

#include <string.h>

// must be included after all other headers
#include "LintFree.h"

namespace
{
	// SQLite primary result codes (sqlite3.h is not part of the DLL's includes).
	const int SQLITE_LOGGER_BUSY = 5;    // SQLITE_BUSY: another connection holds a lock on stats.db
	const int SQLITE_LOGGER_LOCKED = 6;  // SQLITE_LOCKED: a conflicting lock inside this process

	// Retries of a statement that failed on a lock, after the connection's own busy handler has given up
	// (that handler already waits a few milliseconds per attempt inside every execution). 10 retries 20 ms
	// apart add at most about 0.2 s plus the handler's waits per statement. When a statement uses them all
	// up, the rest of that turn gets no retries, so a database locked for a long time cannot stall every row.
	const int SQLITE_LOGGER_MAX_RETRIES = 10;
	const DWORD SQLITE_LOGGER_RETRY_MS = 20;

	bool IsLockError(int iErrorCode)
	{
		const int iPrimary = iErrorCode & 0xFF; // extended result codes carry the primary code in the low byte
		return iPrimary == SQLITE_LOGGER_BUSY || iPrimary == SQLITE_LOGGER_LOCKED;
	}
}

// Reports a failed statement with its table, SQLite error code and message. A macro, so that every call site
// keeps its own assert (an assert that is logged with popups suppressed is ignored at that site from then on).
#define SQLITE_LOGGER_REPORT(pkLogger, szWhat, strTableName, iErrorCode) \
	ASSERT(false, "SqliteLogger: %s (table %s): SQLite error %d: %s", szWhat, std::string(strTableName).c_str(), \
		iErrorCode, (pkLogger)->LastErrorMessage())

//	-----------------------------------------------------------------------------------------------
SqliteLogger::SqliteLogger() :
	m_kConnection(),
	m_bOpenAttempted(false),
	m_bOpen(false),
	m_iNoRetryTurn(-1),
	m_registeredTableSchemas(),
	m_preparedInserts()
{
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::~SqliteLogger()
{
	for (InsertMap::iterator it = m_preparedInserts.begin(); it != m_preparedInserts.end(); ++it)
	{
		delete it->second;
	}
	m_preparedInserts.clear();

	if (m_bOpen)
	{
		m_kConnection.Close();
		m_bOpen = false;
	}
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger& SqliteLogger::getInstance()
{
	static SqliteLogger s_kInstance;
	return s_kInstance;
}

//	-----------------------------------------------------------------------------------------------
const char* SqliteLogger::SqlTypeName(Database::ColumnTypes eType)
{
	switch (eType)
	{
	case Database::COLTYPE_INT:
		return "INTEGER";
	case Database::COLTYPE_FLOAT:
		return "REAL";
	case Database::COLTYPE_TEXT:
		return "TEXT";
	case Database::COLTYPE_BOOL:
		return "BOOLEAN";
	case Database::COLTYPE_NULL:
	default:
		return "NULL";
	}
}

//	-----------------------------------------------------------------------------------------------
bool SqliteLogger::ExecuteOnce(Database::Results* pkResults, const char* szSql)
{
	if (pkResults != NULL && szSql != NULL)
		return m_kConnection.Execute(*pkResults, szSql); // prepare szSql into *pkResults
	if (pkResults != NULL)
		return pkResults->Execute();                     // step the bound statement
	return m_kConnection.Execute(szSql);                 // run szSql to its end
}

//	-----------------------------------------------------------------------------------------------
int SqliteLogger::ExecuteWithRetry(Database::Results* pkResults, const char* szSql)
{
	// A failed sqlite3_step leaves a prepared statement ready to be stepped again with the same bindings (since
	// SQLite 3.7.0 the next step resets it), so a retry repeats the whole insert: rows are neither lost nor doubled.
	if (ExecuteOnce(pkResults, szSql))
		return 0;

	int iErrorCode = m_kConnection.ErrorCode();
	const int iTurn = GC.getGame().getElapsedGameTurns();
	if (!IsLockError(iErrorCode) || m_iNoRetryTurn == iTurn)
		return iErrorCode;

	for (int iRetry = 0; iRetry < SQLITE_LOGGER_MAX_RETRIES; ++iRetry)
	{
		Sleep(SQLITE_LOGGER_RETRY_MS);
		if (ExecuteOnce(pkResults, szSql))
			return 0;

		iErrorCode = m_kConnection.ErrorCode();
		if (!IsLockError(iErrorCode))
			return iErrorCode;
	}

	m_iNoRetryTurn = iTurn;
	return iErrorCode;
}

//	-----------------------------------------------------------------------------------------------
const char* SqliteLogger::LastErrorMessage() const
{
	const char* szMessage = m_kConnection.ErrorMessage();
	return szMessage != NULL ? szMessage : "(no message)";
}

//	-----------------------------------------------------------------------------------------------
bool SqliteLogger::EnsureOpen()
{
	if (!MOD_SQLITE_LOGGING)
		return false;

	if (m_bOpen)
		return true;

	if (m_bOpenAttempted)
		return false;

	m_bOpenAttempted = true;

	CvString strDatabasePath = gDLL->GetCacheFolderPath();
	strDatabasePath += "stats.db";

	if (m_kConnection.Open(strDatabasePath.c_str(), Database::OPEN_CREATE | Database::OPEN_READWRITE | Database::OPEN_FULLMUTEX))
	{
		m_bOpen = true;
		// Don't wait for the disk on every commit: SQLite's defaults flush stats.db and its journal to disk at each
		// commit and create and delete the journal file every time, which stalls the game thread for tens of
		// milliseconds per commit when many games share one disk. Without the flushes a crashed or killed game still
		// leaves a consistent database; only an OS crash or power loss can lose or corrupt the latest rows.
		ExecuteWithRetry(NULL, "PRAGMA synchronous = OFF");
		ExecuteWithRetry(NULL, "PRAGMA journal_mode = PERSIST");
		EnsureDictionaryTable();
	}
	else
	{
		ASSERT(false, "SqliteLogger: failed to open stats.db");
		m_bOpen = false;
	}

	return m_bOpen;
}

//	-----------------------------------------------------------------------------------------------
void SqliteLogger::EnsureDictionaryTable()
{
	if (!m_bOpen)
		return;

	const int iErrorCode = ExecuteWithRetry(NULL,
		"CREATE TABLE IF NOT EXISTS uuid_dictionary ("
		"id INTEGER PRIMARY KEY AUTOINCREMENT, "
		"uuid_hex TEXT UNIQUE NOT NULL)");
	if (iErrorCode != 0)
		SQLITE_LOGGER_REPORT(this, "failed to create table", "uuid_dictionary", iErrorCode);
}

//	-----------------------------------------------------------------------------------------------
int SqliteLogger::ResolveGameId(const std::string& strUuidHex)
{
	if (!EnsureOpen())
		return 0;

	if (strUuidHex.empty())
		return 0;

	// Insert the UUID if it is not already present. The UNIQUE constraint makes this idempotent.
	{
		Database::Results kInsert;
		if (ExecuteWithRetry(&kInsert, "INSERT OR IGNORE INTO uuid_dictionary (uuid_hex) VALUES (?)") == 0)
		{
			kInsert.Bind(1, strUuidHex.c_str());
			const int iErrorCode = ExecuteWithRetry(&kInsert, NULL);
			if (iErrorCode != 0)
				SQLITE_LOGGER_REPORT(this, "failed to record the game id", "uuid_dictionary", iErrorCode);
			kInsert.Reset();
		}
	}

	// Look up the (now guaranteed) row to retrieve its auto-incremented id.
	int iId = 0;
	{
		Database::Results kSelect;
		if (ExecuteWithRetry(&kSelect, "SELECT id FROM uuid_dictionary WHERE uuid_hex = ?") == 0)
		{
			kSelect.Bind(1, strUuidHex.c_str());
			if (kSelect.Step())
				iId = kSelect.GetInt(0);
			kSelect.Reset();
		}
	}

	return iId;
}

//	-----------------------------------------------------------------------------------------------
bool SqliteLogger::IsEnabled()
{
	return EnsureOpen();
}

//	-----------------------------------------------------------------------------------------------
int SqliteLogger::TableSchemaMatches(const std::string& strTableName, const TableDef& kColumns, int& iErrorCode)
{
	std::string strSql = "PRAGMA table_info(\"";
	strSql += strTableName;
	strSql += "\")";

	Database::Results kResults;
	iErrorCode = ExecuteWithRetry(&kResults, strSql.c_str());
	if (iErrorCode != 0)
		return -1;

	size_t uiColumn = 0;
	while (kResults.Step())
	{
		// PRAGMA table_info columns: 0=cid, 1=name, 2=type, ...
		const char* szName = kResults.GetText(1);
		const char* szType = kResults.GetText(2);

		if (uiColumn >= kColumns.size())
		{
			// Live table has more columns than expected.
			kResults.Reset();
			return 0;
		}

		const ColumnDef& kExpected = kColumns[uiColumn];
		if (szName == NULL || _stricmp(szName, kExpected.name.c_str()) != 0)
		{
			kResults.Reset();
			return 0;
		}
		if (szType == NULL || _stricmp(szType, SqlTypeName(kExpected.type)) != 0)
		{
			kResults.Reset();
			return 0;
		}

		++uiColumn;
	}

	// The loop also ends when a step fails: a lock is not "no such table".
	iErrorCode = m_kConnection.ErrorCode();
	kResults.Reset();
	if (IsLockError(iErrorCode))
		return -1;
	iErrorCode = 0;

	// Match only if every expected column was found and there were no extras.
	return (uiColumn == kColumns.size()) && (uiColumn > 0) ? 1 : 0;
}

//	-----------------------------------------------------------------------------------------------
void SqliteLogger::CreateTable(const std::string& strTableName, const TableDef& kColumns)
{
	std::string strSql = "CREATE TABLE \"";
	strSql += strTableName;
	strSql += "\" (";

	for (size_t i = 0; i < kColumns.size(); ++i)
	{
		if (i > 0)
			strSql += ", ";

		strSql += "\"";
		strSql += kColumns[i].name;
		strSql += "\" ";
		strSql += SqlTypeName(kColumns[i].type);
	}

	strSql += ")";

	const int iErrorCode = ExecuteWithRetry(NULL, strSql.c_str());
	if (iErrorCode != 0)
		SQLITE_LOGGER_REPORT(this, "failed to create table", strTableName, iErrorCode);
}

//	-----------------------------------------------------------------------------------------------
void SqliteLogger::RegisterTable(const std::string& strTableName, const TableDef& kColumns)
{
	if (!EnsureOpen())
		return;

	if (kColumns.empty())
	{
		ASSERT(false, "SqliteLogger: cannot register a table with no columns");
		return;
	}

	// Every table implicitly starts with a GameId (INT) and Turn (INT) column. These are
	// prepended to the caller-supplied schema here and bound automatically in BeginLogRow(),
	// so callers must neither declare nor bind them. GameId is an integer id into uuid_dictionary.
	TableDef kFullColumns;
	kFullColumns.reserve(kColumns.size() + 2);
	kFullColumns.push_back(ColumnDef("GameId", Database::COLTYPE_INT));
	kFullColumns.push_back(ColumnDef("Turn", Database::COLTYPE_INT));
	for (size_t i = 0; i < kColumns.size(); ++i)
		kFullColumns.push_back(kColumns[i]);

	// Reject duplicate column names (case-insensitive). This also catches callers that try to
	// re-declare the implicit GameId/Turn columns.
	for (size_t i = 0; i < kFullColumns.size(); ++i)
	{
		for (size_t j = i + 1; j < kFullColumns.size(); ++j)
		{
			if (_stricmp(kFullColumns[i].name.c_str(), kFullColumns[j].name.c_str()) == 0)
			{
				ASSERT(false, "SqliteLogger: duplicate column name in table definition (note: GameId and Turn are implicit and must not be declared)");
				return;
			}
		}
	}

	// Drop any cached prepared insert; it may be stale if we recreate the table below.
	InsertMap::iterator itInsert = m_preparedInserts.find(strTableName);
	if (itInsert != m_preparedInserts.end())
	{
		delete itInsert->second;
		m_preparedInserts.erase(itInsert);
	}

	int iErrorCode = 0;
	const int iMatch = TableSchemaMatches(strTableName, kFullColumns, iErrorCode);
	if (iMatch < 0)
	{
		// The schema could not be read (stats.db stayed locked): never drop a table on that. Register the schema
		// anyway; the inserts then work if the table exists and are reported if it does not.
		SQLITE_LOGGER_REPORT(this, "failed to read the table schema, table left as it is", strTableName, iErrorCode);
	}
	else if (iMatch == 0)
	{
		// Table is missing or has a different schema: drop (if present) and (re)create.
		std::string strDrop = "DROP TABLE IF EXISTS \"";
		strDrop += strTableName;
		strDrop += "\"";
		iErrorCode = ExecuteWithRetry(NULL, strDrop.c_str());
		if (iErrorCode != 0)
			SQLITE_LOGGER_REPORT(this, "failed to drop table", strTableName, iErrorCode);

		CreateTable(strTableName, kFullColumns);
	}

	m_registeredTableSchemas[strTableName] = kFullColumns;
}

//	-----------------------------------------------------------------------------------------------
Database::Results* SqliteLogger::GetOrCreateInsert(const std::string& strTableName)
{
	InsertMap::iterator it = m_preparedInserts.find(strTableName);
	if (it != m_preparedInserts.end())
		return it->second;

	SchemaMap::iterator itSchema = m_registeredTableSchemas.find(strTableName);
	if (itSchema == m_registeredTableSchemas.end())
		return NULL;

	const TableDef& kColumns = itSchema->second;

	std::string strSql = "INSERT INTO \"";
	strSql += strTableName;
	strSql += "\" VALUES (";
	for (size_t i = 0; i < kColumns.size(); ++i)
	{
		if (i > 0)
			strSql += ",";
		strSql += "?";
	}
	strSql += ")";

	Database::Results* pkResults = new Database::Results();
	const int iErrorCode = ExecuteWithRetry(pkResults, strSql.c_str());
	if (iErrorCode != 0)
	{
		SQLITE_LOGGER_REPORT(this, "failed to prepare insert statement, row dropped", strTableName, iErrorCode);
		delete pkResults;
		return NULL;
	}

	m_preparedInserts[strTableName] = pkResults;
	return pkResults;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::Statement SqliteLogger::BeginLogRow(const std::string& strTableName)
{
	if (!EnsureOpen())
		return Statement(this, strTableName, NULL, NULL, false);

	SchemaMap::iterator itSchema = m_registeredTableSchemas.find(strTableName);
	if (itSchema == m_registeredTableSchemas.end())
	{
		ASSERT(false, "SqliteLogger: BeginLogRow called for an unregistered table");
		return Statement(this, strTableName, NULL, NULL, false);
	}

	Database::Results* pkResults = GetOrCreateInsert(strTableName);
	if (pkResults == NULL)
		return Statement(this, strTableName, NULL, NULL, false);

	// Clear any state/bindings from the previous row before starting a new one.
	pkResults->Reset();

	// The implicit GameId and Turn columns lead every table; bind them automatically here so that
	// callers only ever bind their own columns (in declaration order).
	Statement kStatement(this, strTableName, pkResults, &itSchema->second, true);
	kStatement.bind(GC.getGame().getGameDatabaseId());
	kStatement.bind(GC.getGame().getElapsedGameTurns());
	return kStatement;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter SqliteLogger::BeginLogBatch(const std::string& strTableName)
{
	if (!EnsureOpen())
		return BatchWriter(this, strTableName, NULL, NULL, 0, 0, 0, false);

	SchemaMap::iterator itSchema = m_registeredTableSchemas.find(strTableName);
	if (itSchema == m_registeredTableSchemas.end())
	{
		ASSERT(false, "SqliteLogger: BeginLogBatch called for an unregistered table");
		return BatchWriter(this, strTableName, NULL, NULL, 0, 0, 0, false);
	}

	Database::Results* pkResults = GetOrCreateInsert(strTableName);
	if (pkResults == NULL)
		return BatchWriter(this, strTableName, NULL, NULL, 0, 0, 0, false);

	// GameId and Turn are constant for the lifetime of a batch (same game, same turn), so capture
	// them once here instead of re-reading them for every buffered row.
	const int iGameId = GC.getGame().getGameDatabaseId();
	const int iTurn = GC.getGame().getElapsedGameTurns();

	// Higher values buffer more rows in memory before writing, reducing total write time; clamp to
	// at least one row so a misconfigured (zero/negative) value still makes forward progress.
	int iMaxRows = GD_INT_GET(SQLITE_LOGGING_BATCHED_BUFFER_ROWS_MAX);
	if (iMaxRows < 1)
		iMaxRows = 1;

	return BatchWriter(this, strTableName, &itSchema->second, pkResults, iGameId, iTurn, iMaxRows, true);
}

//	===============================================================================================
//	SqliteLogger::Statement
//	===============================================================================================

//	-----------------------------------------------------------------------------------------------
SqliteLogger::Statement::Statement(SqliteLogger* pkOwner, const std::string& strTableName, Database::Results* pkResults, const TableDef* pkSchema, bool bEnabled) :
	m_pkOwner(pkOwner),
	m_strTableName(strTableName),
	m_pkResults(pkResults),
	m_pkSchema(pkSchema),
	m_bEnabled(bEnabled),
	m_bValid(bEnabled),
	m_iBindIndex(0)
{
}

//	-----------------------------------------------------------------------------------------------
bool SqliteLogger::Statement::expectColumn(Database::ColumnTypes eActual)
{
	if (!m_bEnabled || !m_bValid)
		return false;

	if (m_pkSchema == NULL || m_iBindIndex >= (int)m_pkSchema->size())
	{
		ASSERT(false, "SqliteLogger: too many values bound for table");
		m_bValid = false;
		return false;
	}

	const Database::ColumnTypes eExpected = (*m_pkSchema)[m_iBindIndex].type;

	// The bind() overloads normalize float and double to COLTYPE_FLOAT, so an exact match is sufficient.
	const bool bMatch = (eExpected == eActual);

	if (!bMatch)
	{
		ASSERT(false, "SqliteLogger: bound value type does not match the registered column type");
		m_bValid = false;
		return false;
	}

	return true;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::Statement& SqliteLogger::Statement::bind(int iValue)
{
	if (expectColumn(Database::COLTYPE_INT))
	{
		m_pkResults->Bind(m_iBindIndex + 1, iValue);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::Statement& SqliteLogger::Statement::bind(bool bValue)
{
	if (expectColumn(Database::COLTYPE_BOOL))
	{
		m_pkResults->Bind(m_iBindIndex + 1, bValue ? 1 : 0);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::Statement& SqliteLogger::Statement::bind(float fValue)
{
	if (expectColumn(Database::COLTYPE_FLOAT))
	{
		m_pkResults->Bind(m_iBindIndex + 1, fValue);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::Statement& SqliteLogger::Statement::bind(double dValue)
{
	if (expectColumn(Database::COLTYPE_FLOAT))
	{
		m_pkResults->Bind(m_iBindIndex + 1, dValue);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::Statement& SqliteLogger::Statement::bind(const char* szValue)
{
	if (expectColumn(Database::COLTYPE_TEXT))
	{
		m_pkResults->Bind(m_iBindIndex + 1, (szValue != NULL) ? szValue : "");
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::Statement& SqliteLogger::Statement::bind(const std::string& strValue)
{
	if (expectColumn(Database::COLTYPE_TEXT))
	{
		m_pkResults->Bind(m_iBindIndex + 1, strValue.c_str());
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
void SqliteLogger::Statement::execute()
{
	if (!m_bEnabled || !m_bValid || m_pkResults == NULL || m_pkSchema == NULL)
		return;

	if (m_iBindIndex != (int)m_pkSchema->size())
	{
		ASSERT(false, "SqliteLogger: execute() called before all columns were bound");
		return;
	}

	const int iErrorCode = m_pkOwner->ExecuteWithRetry(m_pkResults, NULL);
	if (iErrorCode != 0)
		SQLITE_LOGGER_REPORT(m_pkOwner, "failed to execute insert, row dropped", m_strTableName, iErrorCode);

	// Leave the statement ready for the next row.
	m_pkResults->Reset();
}

//	===============================================================================================
//	SqliteLogger::BatchWriter
//	===============================================================================================

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter::BatchWriter(SqliteLogger* pkOwner, const std::string& strTableName, const TableDef* pkSchema, Database::Results* pkResults, int iGameId, int iTurn, int iMaxRows, bool bEnabled) :
	m_pkOwner(pkOwner),
	m_strTableName(strTableName),
	m_pkSchema(pkSchema),
	m_pkResults(pkResults),
	m_iGameId(iGameId),
	m_iTurn(iTurn),
	m_iMaxRows(iMaxRows),
	m_bEnabled(bEnabled),
	m_buffer(),
	m_scratch()
{
}

//	-----------------------------------------------------------------------------------------------
int SqliteLogger::BatchWriter::CallerColumnCount() const
{
	// Every schema implicitly leads with GameId and Turn, which are not bound per row in a batch.
	if (m_pkSchema == NULL || m_pkSchema->size() < 2)
		return 0;
	return (int)m_pkSchema->size() - 2;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter::RowBuilder SqliteLogger::BatchWriter::BeginLogRow()
{
	// Discard any half-built row left over from a previous (incomplete) BeginLogRow.
	m_scratch.clear();
	return RowBuilder(this, m_bEnabled && m_pkResults != NULL && m_pkSchema != NULL);
}

//	-----------------------------------------------------------------------------------------------
void SqliteLogger::BatchWriter::flush()
{
	if (!m_bEnabled || m_pkResults == NULL || m_pkSchema == NULL)
		return;

	const int iCallerCols = CallerColumnCount();
	if (iCallerCols <= 0 || m_buffer.empty())
	{
		m_buffer.clear();
		return;
	}

	const size_t uiRows = m_buffer.size() / (size_t)iCallerCols;

	// Write every buffered row inside a single transaction so the per-row commit overhead is paid
	// once for the whole batch rather than once per row. IMMEDIATE takes the write lock at the start,
	// where a busy database can be waited for without holding any lock; the inserts then cannot meet
	// another writer, and only the COMMIT can still wait for readers (which SQLite lets us retry).
	int iErrorCode = m_pkOwner->ExecuteWithRetry(NULL, "BEGIN IMMEDIATE");
	if (iErrorCode != 0)
	{
		SQLITE_LOGGER_REPORT(m_pkOwner, "failed to begin a batch, batch dropped", m_strTableName, iErrorCode);
		m_buffer.clear();
		return;
	}

	for (size_t uiRow = 0; uiRow < uiRows; ++uiRow)
	{
		m_pkResults->Reset();
		m_pkResults->Bind(1, m_iGameId);
		m_pkResults->Bind(2, m_iTurn);

		const size_t uiBase = uiRow * (size_t)iCallerCols;
		for (int iCol = 0; iCol < iCallerCols; ++iCol)
		{
			const BatchCell& kCell = m_buffer[uiBase + (size_t)iCol];
			const int iParam = iCol + 3; // 1=GameId, 2=Turn, caller columns follow

			switch (kCell.eType)
			{
			case Database::COLTYPE_TEXT:
				m_pkResults->Bind(iParam, kCell.strValue.c_str());
				break;
			case Database::COLTYPE_FLOAT:
				m_pkResults->Bind(iParam, kCell.dValue);
				break;
			case Database::COLTYPE_BOOL:
			case Database::COLTYPE_INT:
			default:
				m_pkResults->Bind(iParam, kCell.iValue);
				break;
			}
		}

		iErrorCode = m_pkOwner->ExecuteWithRetry(m_pkResults, NULL);
		if (iErrorCode != 0)
			SQLITE_LOGGER_REPORT(m_pkOwner, "failed to execute batched insert, row dropped", m_strTableName, iErrorCode);
	}

	// Intentional: a failed row above is reported but does not abort the batch. For best-effort stats
	// logging, committing the rows that did succeed is preferable to discarding the whole batch on a
	// single bad row. A COMMIT that still fails after its retries is rolled back, so the connection
	// never stays inside a transaction.
	iErrorCode = m_pkOwner->ExecuteWithRetry(NULL, "COMMIT");
	if (iErrorCode != 0)
	{
		SQLITE_LOGGER_REPORT(m_pkOwner, "failed to commit a batch, batch dropped", m_strTableName, iErrorCode);
		m_pkOwner->m_kConnection.RollbackTransaction();
	}

	m_pkResults->Reset();
	m_buffer.clear();
}

//	===============================================================================================
//	SqliteLogger::BatchWriter::RowBuilder
//	===============================================================================================

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter::RowBuilder::RowBuilder(BatchWriter* pkBatch, bool bEnabled) :
	m_pkBatch(pkBatch),
	m_bEnabled(bEnabled),
	m_bValid(bEnabled),
	m_iBindIndex(0)
{
}

//	-----------------------------------------------------------------------------------------------
bool SqliteLogger::BatchWriter::RowBuilder::expectColumn(Database::ColumnTypes eActual)
{
	if (!m_bEnabled || !m_bValid)
		return false;

	const int iCallerCols = m_pkBatch->CallerColumnCount();
	if (m_iBindIndex >= iCallerCols)
	{
		ASSERT(false, "SqliteLogger: too many values bound for batched row");
		m_bValid = false;
		return false;
	}

	// Caller columns start after the implicit GameId/Turn columns in the schema.
	const Database::ColumnTypes eExpected = (*m_pkBatch->m_pkSchema)[m_iBindIndex + 2].type;
	if (eExpected != eActual)
	{
		ASSERT(false, "SqliteLogger: bound value type does not match the registered column type");
		m_bValid = false;
		return false;
	}

	return true;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter::RowBuilder& SqliteLogger::BatchWriter::RowBuilder::bind(int iValue)
{
	if (expectColumn(Database::COLTYPE_INT))
	{
		BatchCell kCell;
		kCell.eType = Database::COLTYPE_INT;
		kCell.iValue = iValue;
		m_pkBatch->m_scratch.push_back(kCell);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter::RowBuilder& SqliteLogger::BatchWriter::RowBuilder::bind(bool bValue)
{
	if (expectColumn(Database::COLTYPE_BOOL))
	{
		BatchCell kCell;
		kCell.eType = Database::COLTYPE_BOOL;
		kCell.iValue = bValue ? 1 : 0;
		m_pkBatch->m_scratch.push_back(kCell);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter::RowBuilder& SqliteLogger::BatchWriter::RowBuilder::bind(float fValue)
{
	if (expectColumn(Database::COLTYPE_FLOAT))
	{
		BatchCell kCell;
		kCell.eType = Database::COLTYPE_FLOAT;
		kCell.dValue = (double)fValue;
		m_pkBatch->m_scratch.push_back(kCell);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter::RowBuilder& SqliteLogger::BatchWriter::RowBuilder::bind(double dValue)
{
	if (expectColumn(Database::COLTYPE_FLOAT))
	{
		BatchCell kCell;
		kCell.eType = Database::COLTYPE_FLOAT;
		kCell.dValue = dValue;
		m_pkBatch->m_scratch.push_back(kCell);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter::RowBuilder& SqliteLogger::BatchWriter::RowBuilder::bind(const char* szValue)
{
	if (expectColumn(Database::COLTYPE_TEXT))
	{
		BatchCell kCell;
		kCell.eType = Database::COLTYPE_TEXT;
		kCell.strValue = (szValue != NULL) ? szValue : "";
		m_pkBatch->m_scratch.push_back(kCell);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter::RowBuilder& SqliteLogger::BatchWriter::RowBuilder::bind(const std::string& strValue)
{
	if (expectColumn(Database::COLTYPE_TEXT))
	{
		BatchCell kCell;
		kCell.eType = Database::COLTYPE_TEXT;
		kCell.strValue = strValue;
		m_pkBatch->m_scratch.push_back(kCell);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
void SqliteLogger::BatchWriter::RowBuilder::addRowToBatch()
{
	if (!m_bEnabled || !m_bValid)
	{
		m_pkBatch->m_scratch.clear();
		return;
	}

	if (m_iBindIndex != m_pkBatch->CallerColumnCount())
	{
		ASSERT(false, "SqliteLogger: addRowToBatch() called before all columns were bound");
		m_pkBatch->m_scratch.clear();
		return;
	}

	// Move the completed row's cells into the flat batch buffer, then reset the scratch row.
	for (size_t i = 0; i < m_pkBatch->m_scratch.size(); ++i)
		m_pkBatch->m_buffer.push_back(m_pkBatch->m_scratch[i]);
	m_pkBatch->m_scratch.clear();

	// Auto-flush once the buffer reaches the configured row cap to bound peak memory usage.
	const int iCallerCols = m_pkBatch->CallerColumnCount();
	if (iCallerCols > 0)
	{
		const size_t uiRows = m_pkBatch->m_buffer.size() / (size_t)iCallerCols;
		if (uiRows >= (size_t)m_pkBatch->m_iMaxRows)
			m_pkBatch->flush();
	}
}

