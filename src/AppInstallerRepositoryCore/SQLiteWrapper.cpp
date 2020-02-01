// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "SQLiteWrapper.h"

#include <wil/result_macros.h>

using namespace std::string_view_literals;

// TODO: Invoke the wil error handling callback to log the error
#define THROW_SQLITE(_error_) \
    do { \
        int _ts_sqliteReturnValue = _error_; \
        THROW_EXCEPTION_MSG(SQLiteException(_ts_sqliteReturnValue), sqlite3_errstr(_ts_sqliteReturnValue)); \
    } while (0,0)

#define THROW_IF_SQLITE_FAILED(_statement_) \
    do { \
        int _tisf_sqliteReturnValue = _statement_; \
        if (_tisf_sqliteReturnValue != SQLITE_OK) \
        { \
            THROW_SQLITE(_tisf_sqliteReturnValue); \
        } \
    } while (0,0)

namespace AppInstaller::Repository::SQLite
{
    std::string_view RowIDName = "rowid"sv;

    namespace
    {
        size_t GetNextStatementId()
        {
            static std::atomic_size_t statementId(0);
            return ++statementId;
        }
    }

    namespace details
    {
        void ParameterSpecificsImpl<nullptr_t>::Bind(sqlite3_stmt* stmt, int index, nullptr_t)
        {
            THROW_IF_SQLITE_FAILED(sqlite3_bind_null(stmt, index));
        }

        void ParameterSpecificsImpl<std::string>::Bind(sqlite3_stmt* stmt, int index, const std::string& v)
        {
            THROW_IF_SQLITE_FAILED(sqlite3_bind_text64(stmt, index, v.c_str(), v.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
        }

        std::string ParameterSpecificsImpl<std::string>::GetColumn(sqlite3_stmt* stmt, int column)
        {
            return reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
        }

        void ParameterSpecificsImpl<std::string_view>::Bind(sqlite3_stmt* stmt, int index, std::string_view v)
        {
            THROW_IF_SQLITE_FAILED(sqlite3_bind_text64(stmt, index, v.data(), v.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
        }

        void ParameterSpecificsImpl<int>::Bind(sqlite3_stmt* stmt, int index, int v)
        {
            THROW_IF_SQLITE_FAILED(sqlite3_bind_int(stmt, index, v));
        }

        int ParameterSpecificsImpl<int>::GetColumn(sqlite3_stmt* stmt, int column)
        {
            return sqlite3_column_int(stmt, column);
        }

        void ParameterSpecificsImpl<int64_t>::Bind(sqlite3_stmt* stmt, int index, int64_t v)
        {
            THROW_IF_SQLITE_FAILED(sqlite3_bind_int64(stmt, index, v));
        }

        int64_t ParameterSpecificsImpl<int64_t>::GetColumn(sqlite3_stmt* stmt, int column)
        {
            return sqlite3_column_int64(stmt, column);
        }
    }

    Connection::Connection(const std::string& target, OpenDisposition disposition, OpenFlags flags)
    {
        AICLI_LOG(SQL, Info, << "Opening SQLite connection: '" << target << "' [" << std::hex << static_cast<int>(disposition) << ", " << std::hex << static_cast<int>(flags) << "]");
        int resultingFlags = static_cast<int>(disposition) | static_cast<int>(flags);
        THROW_IF_SQLITE_FAILED(sqlite3_open_v2(target.c_str(), &m_dbconn, resultingFlags, nullptr));
    }

    Connection Connection::Create(const std::string& target, OpenDisposition disposition, OpenFlags flags)
    {
        Connection result{ target, disposition, flags };
        
        THROW_IF_SQLITE_FAILED(sqlite3_extended_result_codes(result.m_dbconn.get(), 1));

        return result;
    }

    int64_t Connection::GetLastInsertRowID()
    {
        return sqlite3_last_insert_rowid(m_dbconn.get());
    }

    Statement::Statement(Connection& connection, std::string_view sql, bool persistent)
    {
        m_id = GetNextStatementId();
        AICLI_LOG(SQL, Verbose, << "Preparing statement #" << m_id << ": " << sql);
        // SQL string size should include the null terminator (https://www.sqlite.org/c3ref/prepare.html)
        assert(sql.data()[sql.size()] == '\0');
        THROW_IF_SQLITE_FAILED(sqlite3_prepare_v3(connection, sql.data(), static_cast<int>(sql.size() + 1), (persistent ? SQLITE_PREPARE_PERSISTENT : 0), &m_stmt, nullptr));
    }

    Statement Statement::Create(Connection& connection, const std::string& sql, bool persistent)
    {
        return { connection, { sql.c_str(), sql.size() }, persistent };
    }

    Statement Statement::Create(Connection& connection, std::string_view sql, bool persistent)
    {
        // We need the statement to be null terminated, and the only way to guarantee that with a string_view is to construct a string copy.
        return Create(connection, std::string(sql), persistent);
    }

    Statement Statement::Create(Connection& connection, char const* const sql, bool persistent)
    {
        return { connection, sql, persistent };
    }

    bool Statement::Step(bool failFastOnError)
    {
        AICLI_LOG(SQL, Verbose, << "Stepping statement #" << m_id);
        int result = sqlite3_step(m_stmt.get());

        if (result == SQLITE_ROW)
        {
            AICLI_LOG(SQL, Verbose, << "Statement #" << m_id << " has data");
            m_state = State::HasRow;
            return true;
        }
        else if (result == SQLITE_DONE)
        {
            AICLI_LOG(SQL, Verbose, << "Statement #" << m_id << " has completed");
            m_state = State::Completed;
            return false;
        }
        else
        {
            m_state = State::Error;
            if (failFastOnError)
            {
                FAIL_FAST_MSG("Critical SQL statement failed");
            }
            else
            {
                THROW_SQLITE(result);
            }
        }
    }

    void Statement::Execute(bool failFastOnError)
    {
        THROW_HR_IF(E_UNEXPECTED, Step(failFastOnError));
    }

    bool Statement::GetColumnIsNull(int column)
    {
        int type = sqlite3_column_type(m_stmt.get(), column);
        return type == SQLITE_NULL;
    }

    void Statement::Reset()
    {
        AICLI_LOG(SQL, Verbose, << "Reset statement #" << m_id);
        // Ignore return value from reset, as if it is an error, it was the error from the last call to step.
        sqlite3_reset(m_stmt.get());
        m_state = State::Prepared;
    }

    Savepoint::Savepoint(Connection& connection, std::string&& name) :
        m_name(std::move(name))
    {
        using namespace std::string_literals;

        Statement begin = Statement::Create(connection, "SAVEPOINT ["s + m_name + "]");
        m_rollback = Statement::Create(connection, "ROLLBACK TO ["s + m_name + "]", true);
        m_commit = Statement::Create(connection, "RELEASE ["s + m_name + "]", true);

        AICLI_LOG(SQL, Info, << "Begin savepoint: " << m_name);
        begin.Step();
    }

    Savepoint Savepoint::Create(Connection& connection, std::string name)
    {
        return { connection, std::move(name) };
    }

    Savepoint::~Savepoint()
    {
        Rollback();
    }

    void Savepoint::Rollback()
    {
        if (m_inProgress)
        {
            AICLI_LOG(SQL, Info, << "Roll back savepoint: " << m_name);
            m_rollback.Step(true);
            m_inProgress = false;
        }
    }

    void Savepoint::Commit()
    {
        if (m_inProgress)
        {
            AICLI_LOG(SQL, Info, << "Commit savepoint: " << m_name);
            m_commit.Step(true);
            m_inProgress = false;
        }
    }
}