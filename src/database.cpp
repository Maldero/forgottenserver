/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2015  Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "otpch.h"

#include "configmanager.h"
#include "database.h"

#include <errmsg.h>

extern ConfigManager g_config;

Database::Database()
{
	m_handle = nullptr;
	maxPacketSize = 16777216;
}

Database::~Database()
{
	if (m_handle != nullptr) {
		mysql_close(m_handle);
	}
}

bool Database::connect()
{
	// connection handle initialization
	m_handle = mysql_init(nullptr);
	if (!m_handle) {
		std::cout << std::endl << "Failed to initialize MySQL connection handle." << std::endl;
		return false;
	}

	// automatic reconnect
	my_bool reconnect = true;
	mysql_options(m_handle, MYSQL_OPT_RECONNECT, &reconnect);

	// connects to database
	if (!mysql_real_connect(m_handle, g_config.getString(ConfigManager::MYSQL_HOST).c_str(), g_config.getString(ConfigManager::MYSQL_USER).c_str(), g_config.getString(ConfigManager::MYSQL_PASS).c_str(), g_config.getString(ConfigManager::MYSQL_DB).c_str(), g_config.getNumber(ConfigManager::SQL_PORT), g_config.getString(ConfigManager::MYSQL_SOCK).c_str(), 0)) {
		std::cout << std::endl << "MySQL Error Message: " << mysql_error(m_handle) << std::endl;
		return false;
	}

	DBResult_ptr result = storeQuery("SHOW VARIABLES LIKE 'max_allowed_packet'");
	if (result) {
		maxPacketSize = result->getNumber<uint64_t>("Value");
	}
	return true;
}

bool Database::beginTransaction()
{
	if (!executeQuery("BEGIN")) {
		return false;
	}

	database_lock.lock();
	return true;
}

bool Database::rollback()
{
	if (mysql_rollback(m_handle) != 0) {
		std::cout << "[Error - mysql_rollback] Message: " << mysql_error(m_handle) << std::endl;
		database_lock.unlock();
		return false;
	}

	database_lock.unlock();
	return true;
}

bool Database::commit()
{
	if (mysql_commit(m_handle) != 0) {
		std::cout << "[Error - mysql_commit] Message: " << mysql_error(m_handle) << std::endl;
		database_lock.unlock();
		return false;
	}

	database_lock.unlock();
	return true;
}

bool Database::executeQuery(const std::string& query)
{
	bool success = true;

	// executes the query
	database_lock.lock();

	while (mysql_real_query(m_handle, query.c_str(), query.length()) != 0) {
		std::cout << "[Error - mysql_real_query] Query: " << query.substr(0, 256) << std::endl << "Message: " << mysql_error(m_handle) << std::endl;
		auto error = mysql_errno(m_handle);
		if (error != CR_SERVER_LOST && error != CR_SERVER_GONE_ERROR && error != CR_CONN_HOST_ERROR && error != 1053/*ER_SERVER_SHUTDOWN*/ && error != CR_CONNECTION_ERROR) {
			success = false;
			break;
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	MYSQL_RES* m_res = mysql_store_result(m_handle);
	database_lock.unlock();

	if (m_res) {
		mysql_free_result(m_res);
	}

	return success;
}

DBResult_ptr Database::storeQuery(const std::string& query)
{
	// executes the query
	database_lock.lock();

	retry:
	while (mysql_real_query(m_handle, query.c_str(), query.length()) != 0) {
		std::cout << "[Error - mysql_real_query] Query: " << query << std::endl << "Message: " << mysql_error(m_handle) << std::endl;
		auto error = mysql_errno(m_handle);
		if (error != CR_SERVER_LOST && error != CR_SERVER_GONE_ERROR && error != CR_CONN_HOST_ERROR && error != 1053/*ER_SERVER_SHUTDOWN*/ && error != CR_CONNECTION_ERROR) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	// we should call that every time as someone would call executeQuery('SELECT...')
	// as it is described in MySQL manual: "it doesn't hurt" :P
	MYSQL_RES* m_res = mysql_store_result(m_handle);

	// error occurred
	if (!m_res) {
		std::cout << "[Error - mysql_store_result] Query: " << query << std::endl << "Message: " << mysql_error(m_handle) << std::endl;
		int error = mysql_errno(m_handle);
		if (error != CR_SERVER_LOST && error != CR_SERVER_GONE_ERROR && error != CR_CONN_HOST_ERROR && error != 1053/*ER_SERVER_SHUTDOWN*/ && error != CR_CONNECTION_ERROR) {
			database_lock.unlock();
			return nullptr;
		}
		goto retry;
	}
	database_lock.unlock();

	// retrieving results of query
	DBResult_ptr result = std::make_shared<DBResult>(m_res);
	if (!result->hasNext()) {
		return nullptr;
	}
	return result;
}

std::string Database::escapeString(const std::string& s) const
{
	return escapeBlob(s.c_str(), s.length());
}

std::string Database::escapeBlob(const char* s, uint32_t length) const
{
	// the worst case is 2n + 1
	size_t maxLength = (length * 2) + 1;

	std::string escaped;
	escaped.reserve(maxLength + 2);
	escaped.push_back('\'');

	if (length != 0) {
		char* output = new char[maxLength];
		mysql_real_escape_string(m_handle, output, s, length);
		escaped.append(output);
		delete[] output;
	}

	escaped.push_back('\'');
	return escaped;
}

DBResult::DBResult(MYSQL_RES* res)
{
	m_handle = res;

	int32_t i = 0;

	MYSQL_FIELD* field = mysql_fetch_field(m_handle);
	while (field) {
		m_listNames[field->name] = i++;
		field = mysql_fetch_field(m_handle);
	}

	m_row = mysql_fetch_row(m_handle);
}

DBResult::~DBResult()
{
	mysql_free_result(m_handle);
}

int32_t DBResult::getDataInt(const std::string& s) const
{
	auto it = m_listNames.find(s);
	if (it == m_listNames.end()) {
		std::cout << "[Error - DBResult::getDataInt] Column '" << s << "' does not exist in result set." << std::endl;
		return 0;
	}

	if (m_row[it->second] == nullptr) {
		return 0;
	}

	return atoi(m_row[it->second]);
}

std::string DBResult::getDataString(const std::string& s) const
{
	auto it = m_listNames.find(s);
	if (it == m_listNames.end()) {
		std::cout << "[Error - DBResult::getDataString] Column '" << s << "' does not exist in result set." << std::endl;
		return std::string();
	}

	if (m_row[it->second] == nullptr) {
		return std::string();
	}

	return std::string(m_row[it->second]);
}

const char* DBResult::getDataStream(const std::string& s, unsigned long& size) const
{
	auto it = m_listNames.find(s);
	if (it == m_listNames.end()) {
		std::cout << "[Error - DBResult::getDataStream] Column '" << s << "' does not exist in result set." << std::endl;
		size = 0;
		return nullptr;
	}

	if (m_row[it->second] == nullptr) {
		size = 0;
		return nullptr;
	}

	size = mysql_fetch_lengths(m_handle)[it->second];
	return m_row[it->second];
}

bool DBResult::hasNext() const
{
	return m_row != nullptr;
}

bool DBResult::next()
{
	m_row = mysql_fetch_row(m_handle);
	return m_row != nullptr;
}

DBInsert::DBInsert(std::string query) : query(query)
{
	this->length = this->query.length();
}

bool DBInsert::addRow(const std::string& row)
{
	// adds new row to buffer
	const size_t rowLength = row.length();
	length += rowLength;
	if (length > Database::getInstance()->getMaxPacketSize() && !execute()) {
		return false;
	}

	if (values.empty()) {
		values.reserve(rowLength + 2);
		values.push_back('(');
		values.append(row);
		values.push_back(')');
	} else {
		values.reserve(values.length() + rowLength + 3);
		values.push_back(',');
		values.push_back('(');
		values.append(row);
		values.push_back(')');
	}
	return true;
}

bool DBInsert::addRow(std::ostringstream& row)
{
	bool ret = addRow(row.str());
	row.str(std::string());
	return ret;
}

bool DBInsert::execute()
{
	if (values.empty()) {
		return true;
	}

	// executes buffer
	bool res = Database::getInstance()->executeQuery(query + values);
	values.clear();
	length = query.length();
	return res;
}
