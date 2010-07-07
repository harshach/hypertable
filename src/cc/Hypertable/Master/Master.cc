/** -*- c++ -*-
 * Copyright (C) 2009 Doug Judd (Zvents, Inc.)
 *
 * This file is part of Hypertable.
 *
 * Hypertable is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2 of the
 * License, or any later version.
 *
 * Hypertable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "Common/Compat.h"

#include <algorithm>
#include <cstdlib>

#include <iostream>
#include <fstream>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

#include "Common/FileUtils.h"
#include "Common/Path.h"
#include "Common/InetAddr.h"
#include "Common/SystemInfo.h"

#include "DfsBroker/Lib/Client.h"
#include "Hypertable/Lib/LocationCache.h"
#include "Hypertable/Lib/RangeServerClient.h"
#include "Hypertable/Lib/RangeState.h"
#include "Hypertable/Lib/Schema.h"
#include "Hypertable/Lib/Client.h"
#include "Hyperspace/DirEntry.h"

#include "DispatchHandlerDropTable.h"
#include "DispatchHandlerUpdateSchema.h"
#include "DispatchHandlerGetStatistics.h"

#include "Master.h"
#include "ServersDirectoryHandler.h"
#include "ServerLockFileHandler.h"
#include "RangeServerState.h"

using namespace Hyperspace;
using namespace Hypertable;
using namespace Hypertable::DfsBroker;
using namespace std;

namespace Hypertable {

String Master::ms_monitoring_dir;

Master::Master(PropertiesPtr &props, ConnectionManagerPtr &conn_mgr,
               ApplicationQueuePtr &app_queue)
  : m_props_ptr(props), m_conn_manager_ptr(conn_mgr),
    m_app_queue_ptr(app_queue), m_verbose(false), m_dfs_client(0),
    m_next_server_id(1), m_initialized(false), m_root_server_connected(false), m_get_stats_outstanding(false) {

  m_server_map_iter = m_server_map.begin();

  m_hyperspace_ptr = new Hyperspace::Session(conn_mgr->get_comm(), props);
  m_hyperspace_ptr->add_callback(&m_hyperspace_session_handler);
  uint32_t timeout = props->get_i32("Hyperspace.Timeout");

  if (!m_hyperspace_ptr->wait_for_connection(timeout)) {
    HT_ERROR("Unable to connect to hyperspace, exiting...");
    exit(1);
  }

  m_verbose = props->get_bool("Hypertable.Verbose");
  uint16_t port = props->get_i16("Hypertable.Master.Port");
  m_max_range_bytes = props->get_i64("Hypertable.RangeServer.Range.SplitSize");

  // store 10 mins worth of stats and at least last 5 stat buffers
  int stats_int = props->get_i32("Hypertable.Master.StatsGather.Interval");
  int stats_buffer_size = 10*60000/stats_int;
  stats_buffer_size= std::max(stats_buffer_size, 5);
  m_table_stats_buffer.set_capacity(stats_buffer_size);
  m_range_server_stats_buffer.set_capacity(stats_buffer_size);

  /**
   * Create DFS Client connection
   */
  DfsBroker::Client *dfs_client = new DfsBroker::Client(conn_mgr, props);

  int dfs_timeout;
  if (props->has("DfsBroker.Timeout"))
    dfs_timeout = props->get_i32("DfsBroker.Timeout");
  else
    dfs_timeout = props->get_i32("Hypertable.Request.Timeout");

  if (!dfs_client->wait_for_connection(dfs_timeout)) {
    HT_ERROR("Unable to connect to DFS Broker, exiting...");
    exit(1);
  }
  m_dfs_client = dfs_client;

  atomic_set(&m_last_table_id, 0);

  if (!initialize())
    exit(1);

  /* Read Last Table ID */
  {
    DynamicBuffer valbuf(0);
    HandleCallbackPtr null_handle_callback;
    int ival;
    uint32_t lock_status;
    uint32_t oflags = OPEN_FLAG_READ | OPEN_FLAG_WRITE | OPEN_FLAG_LOCK;

    m_master_file_handle = m_hyperspace_ptr->open("/hypertable/master", oflags,
                                                  null_handle_callback);

    m_hyperspace_ptr->try_lock(m_master_file_handle, LOCK_MODE_EXCLUSIVE,
                               &lock_status, &m_master_file_sequencer);

    if (lock_status != LOCK_STATUS_GRANTED) {
      HT_ERROR("Unable to obtain lock on '/hypertable/master' - conflict");
      exit(1);
    }

    // Write master location in 'address' attribute, format is IP:port
    InetAddr addr(System::net_info().primary_addr, port);
    String addr_s = addr.format();
    m_hyperspace_ptr->attr_set(m_master_file_handle, "address",
                               addr_s.c_str(), addr_s.length());

    try {
      m_hyperspace_ptr->attr_get(m_master_file_handle, "last_table_id", valbuf);
      ival = atoi((const char *)valbuf.base);
    }
    catch (Exception &e) {
      if (e.code() == Error::HYPERSPACE_ATTR_NOT_FOUND) {
        m_hyperspace_ptr->attr_set(m_master_file_handle, "last_table_id", "0", 2);
        ival = 0;
      }
      else
        HT_THROW2(e.code(), e, e.what());
    }
    atomic_set(&m_last_table_id, ival);
    if (m_verbose)
      cout << "Last Table ID: " << ival << endl;

    try {
      m_hyperspace_ptr->attr_get(m_master_file_handle, "next_server_id", valbuf);
      ival = atoi((const char *)valbuf.base);
    }
    catch (Exception &e) {
      if (e.code() == Error::HYPERSPACE_ATTR_NOT_FOUND) {
        m_hyperspace_ptr->attr_set(m_master_file_handle, "next_server_id", "1", 2);
        ival = 1;
      }
      else
        HT_THROW2(e.code(), e, e.what());
    }
    m_next_server_id = (uint32_t)ival;
  }

  /**
   * Locate tablet servers
   */
  scan_servers_directory();

  master_gc_start(props, m_threads, m_metadata_table_ptr, m_dfs_client);


}

Master::~Master() {
  delete m_dfs_client;
}



/**
 *
 */
void Master::server_joined(const String &location) {
  HT_INFOF("Server Joined (%s)", location.c_str());
  cout << flush;
}



/**
 *
 */
void Master::server_left(const String &location) {
  LockSequencer lock_sequencer;
  String hsfname = (String)"/hypertable/servers/" + location;
  InetAddr connection;
  bool was_connected = false;

  HT_INFOF("Server left: %s", location.c_str());

  {
    ScopedLock lock(m_mutex);
    RangeServerStatePtr rs_state;
    RangeServerStateMap::iterator iter = m_server_map.find(location);

    {
      ScopedLock init_lock(m_root_server_mutex);
      if (location == m_root_server_location) {
	m_root_server_connected = false;
	memset(&m_root_server_addr, 0, sizeof(m_root_server_addr));
      }
    }

    if (iter == m_server_map.end()) {
      HT_WARNF("Server (%s) not found in map", location.c_str());
      return;
    }

    rs_state = (*iter).second;

    // if we're about to delete the item pointing to the server map iterator,
    // then advance the iterator
    if (iter == m_server_map_iter)
      ++m_server_map_iter;

    m_addr_map.erase(rs_state->connection);
    m_server_map.erase(iter);

    if (m_server_map.empty())
      m_no_servers_cond.notify_all();

    connection = rs_state->connection;
    was_connected = rs_state->connected;
    rs_state->connected = false;
  }

  if (was_connected)
    m_conn_manager_ptr->get_comm()->close_socket(connection);

  // delete Hyperspace file
  HT_INFOF("RangeServer lost it's lock on file %s, deleting ...", hsfname.c_str());
  try {
    m_hyperspace_ptr->unlink(hsfname);
  }
  catch (Exception &e) {
    HT_WARN_OUT "Problem closing file '" << hsfname << "' - " << e << HT_END;
  }

  /**
   *  Do (or schedule) tablet re-assignment here
   */
}



/**
 *
 */
void
Master::create_table(ResponseCallback *cb, const char *tablename,
                     const char *schemastr) {

  HT_INFOF("Create table: %s", tablename);
  wait_for_root_metadata_server();

  try {
    create_table(tablename, schemastr);
  }
  catch (Exception &e) {
    HT_ERROR_OUT << e << HT_END;
    cb->error(e.code(), e.what());
    return;
  }

  cb->response_ok();
}

namespace {
  typedef hash_map<String, InetAddr> ConnectionMap;
}

/**
 *
 */
void
Master::alter_table(ResponseCallback *cb, const char *tablename,
                    const char *schemastr) {
  String finalschema = "";
  String err_msg = "";
  String tablefile = (String)"/hypertable/tables/" + tablename;
  SchemaPtr updated_schema;
  SchemaPtr schema;
  DynamicBuffer value_buf(0);
  uint64_t handle = 0;
  LockSequencer lock_sequencer;
  int ival=0;
  int saved_error = Error::OK;

  HandleCallbackPtr null_handle_callback;

  HT_INFOF("Alter table: %s", tablename);

  wait_for_root_metadata_server();

  try {
    /**
     * Check for table existence
     */
    if (!m_hyperspace_ptr->exists(tablefile)) {
      HT_THROW(Error::TABLE_NOT_FOUND, tablename);
    }

    /**
     *  Parse new schema & check validity
     */
    updated_schema = Schema::new_instance(schemastr, strlen(schemastr),
        true);
    if (!updated_schema->is_valid())
      HT_THROW(Error::MASTER_BAD_SCHEMA, updated_schema->get_error_string());

    /**
     *  Open & Lock Hyperspace file exclusively
     */
    handle = m_hyperspace_ptr->open(tablefile,
        OPEN_FLAG_READ|OPEN_FLAG_WRITE|OPEN_FLAG_LOCK_EXCLUSIVE,
        null_handle_callback);

    /**
     *  Read existing schema and table id
     */

    m_hyperspace_ptr->attr_get(handle, "schema", value_buf);
    schema = Schema::new_instance((char *)value_buf.base,
        strlen((char *)value_buf.base), true);
    value_buf.clear();
    m_hyperspace_ptr->attr_get(handle, "table_id", value_buf);
    ival = atoi((const char *)value_buf.base);

    /**
     * Check if proposed schema generation is correct
     */
    uint32_t generation =  schema->get_generation()+1;
    if (updated_schema->get_generation() != generation) {
      HT_THROW(Error::MASTER_SCHEMA_GENERATION_MISMATCH,
          (String) "Expected updated schema generation " + generation
          + " got " + updated_schema->get_generation());
    }

    /**
     * Send updated schema to all RangeServers handling this table
     */
    finalschema = schemastr;
    {
      char start_row[16];
      char end_row[16];
      TableScannerPtr scanner_ptr;
      ScanSpec scan_spec;
      Cell cell;
      String location_str;
      ConnectionMap connections;
      ConnectionMap::iterator cmiter;
      RangeServerStateMap::iterator smiter;
      TableIdentifier table;
      RowInterval ri;

      table.name = tablename;
      table.id = ival;
      table.generation = 0;

      sprintf(start_row, "%d:", ival);
      sprintf(end_row, "%d:%s", ival, Key::END_ROW_MARKER);

      scan_spec.row_limit = 0;
      scan_spec.max_versions = 1;
      scan_spec.columns.clear();
      scan_spec.columns.push_back("Location");

      ri.start = start_row;
      ri.end = end_row;
      scan_spec.row_intervals.push_back(ri);

      scanner_ptr = m_metadata_table_ptr->create_scanner(scan_spec);

      while (scanner_ptr->next(cell)) {
	location_str = String((const char *)cell.value, cell.value_len);
	boost::trim(location_str);
	if (connections.find(location_str) == connections.end()) {
	  ScopedLock lock(m_mutex);
	  if ((smiter = m_server_map.find(location_str)) == m_server_map.end()) {
	    /** Alter failed clean up & return **/
	    saved_error = Error::RANGESERVER_UNAVAILABLE;
	    err_msg = location_str;
	    HT_ERRORF("ALTER TABLE failed '%s' - %s", err_msg.c_str(),
		      Error::get_text(saved_error));
	    break;
	  }
	  connections[location_str] = (*smiter).second->connection;
	}
      }

      if (saved_error != Error::OK) {
	cb->error(saved_error, err_msg);
	m_hyperspace_ptr->close(handle);
	return;
      }

      if (!connections.empty()) {
        DispatchHandlerUpdateSchema sync_handler(table,
            finalschema.c_str(), m_conn_manager_ptr->get_comm(), 5000);
        RangeServerStatePtr state_ptr;

	// Issue ALTER TABLE commands to RangeServers
	for (cmiter = connections.begin(); cmiter != connections.end(); ++cmiter) {
	  CommAddress addr;
	  addr.set_proxy((*cmiter).first);
	  sync_handler.add((*cmiter).second);
	}

        if (!sync_handler.wait_for_completion()) {
          std::vector<DispatchHandlerUpdateSchema::ErrorResult> errors;
          uint32_t retry_count = 0;
          bool retry_failed;
          do {
            retry_count++;
            sync_handler.get_errors(errors);
            for (size_t i=0; i<errors.size(); i++) {
              HT_ERRORF("update schema error - %s - %s",
                  errors[i].msg.c_str(), Error::get_text(errors[i].error));
            }
            sync_handler.retry();
          }
          while ((retry_failed = (!sync_handler.wait_for_completion())) &&
              retry_count < MAX_ALTER_TABLE_RETRIES);
          /**
           * Alter table failed.. die for now
           */
          if (retry_failed) {
            sync_handler.get_errors(errors);
            String error_str;
            for (size_t i=0; i<errors.size(); i++) {
              error_str += (String) "update schema error '" +
                  errors[i].msg.c_str() + "' '" +
                  Error::get_text(errors[i].error) + "'";
              HT_FATALF("Maximum alter table attempts reached %d - %s",
                  MAX_ALTER_TABLE_RETRIES, error_str.c_str());
            }
          }
        }
      }

      /**
       * Store updated Schema in Hyperspace, close handle & release lock
       */
      HT_INFO_OUT <<"schema:\n"<< finalschema << HT_END;
      m_hyperspace_ptr->attr_set(handle, "schema", finalschema.c_str(),
                                 finalschema.length());
      /**
       * Alter succeeded so clean up!
       */
      m_hyperspace_ptr->close(handle);

      HT_INFOF("ALTER TABLE '%s' id=%d success",
          tablename, ival);
    }
  }
  catch (Exception &e) {
    // clean up
    if(handle != 0)
      m_hyperspace_ptr->close(handle);

    HT_ERROR_OUT << e << HT_END;
    cb->error(e.code(), e.what());
    return;
  }

  cb->response_ok();
}

/**
 *
 */
void Master::get_schema(ResponseCallbackGetSchema *cb, const char *tablename) {
  String tablefile = (String)"/hypertable/tables/" + tablename;
  String errmsg;
  DynamicBuffer schemabuf(0);
  uint64_t handle;
  HandleCallbackPtr null_handle_callback;

  HT_INFOF("Get schema: %s", tablename);

  wait_for_root_metadata_server();

  try {

    /**
     * Check for table existence
     */
    if (!m_hyperspace_ptr->exists(tablefile)) {
      cb->error(Error::TABLE_NOT_FOUND, tablename);
      return;
    }

    /**
     * Open table file
     */
    handle = m_hyperspace_ptr->open(tablefile, OPEN_FLAG_READ,
                                    null_handle_callback);

    /**
     * Get schema attribute
     */
    m_hyperspace_ptr->attr_get(handle, "schema", schemabuf);

    m_hyperspace_ptr->close(handle);

    cb->response((char *)schemabuf.base);

    if (m_verbose) {
      HT_INFOF("Successfully fetched schema (length=%d) for table '%s'",
               (int)strlen((char *)schemabuf.base), tablename);
    }

  }
  catch (Exception &e) {
    HT_ERROR_OUT << e << HT_END;
    cb->error(e.code(), e.what());
  }
}



/**
 *
 */
void
Master::register_server(ResponseCallbackRegisterServer *cb, String &location,
                        const InetAddr &addr) {
  RangeServerStateMap::iterator iter;
  HandleCallbackPtr lock_file_handler;
  LockSequencer lock_sequencer;
  String hsfname;
  bool exists = false;
  InetAddr connection = cb->get_address();

  if (location == "") {
    {
      ScopedLock lock(m_mutex);
      location = String("rs") + m_next_server_id++;
    }
    char buf[16];
    sprintf(buf, "%u", m_next_server_id);
    m_hyperspace_ptr->attr_set(m_master_file_handle, "next_server_id",
                               buf, strlen(buf)+1);
    String addr_str = InetAddr::format(addr);
    m_hyperspace_ptr->attr_set(m_servers_dir_handle, location,
                               addr_str.c_str(), addr_str.length()+1);
  }

  HT_INFOF("Register server %s (%s -> %s)", connection.format().c_str(),
           location.c_str(), InetAddr::format(addr).c_str());

  try {

    {
      ScopedLock lock(m_mutex);
      RangeServerStatePtr rs_state;

      if((iter = m_server_map.find(location)) != m_server_map.end()) {
	rs_state = (*iter).second;
	HT_FATALF("Rangeserver at %s and %s both assigned location '%s', aborting...",
                  InetAddr::format(addr).c_str(), InetAddr::format(rs_state->connection).c_str(),
                  location.c_str());
      }
      else {
	rs_state = new RangeServerState();
	rs_state->location = location;
	rs_state->addr.set_proxy(location);
      }

      m_conn_manager_ptr->get_comm()->set_alias(connection, addr);
      m_conn_manager_ptr->get_comm()->add_proxy(location, addr);

      rs_state->connection = connection;
      rs_state->connected = true;

      hsfname = (String)"/hypertable/servers/" + location;

      m_server_map[rs_state->location] = rs_state;
      m_addr_map[rs_state->connection] = location;
    }

    HT_INFOF("Server Registered %s -> %s", location.c_str(),
             InetAddr::format(addr).c_str());

    cb->response(location);

  }
  catch (Exception &e) {
    HT_ERROR_OUT << e << HT_END;
    cb->error(e.code(), e.what());
    return;
  }

  /**
   * TEMPORARY: Load root and second-level METADATA ranges
   */
  {
    ScopedLock init_lock(m_root_server_mutex);

    if (!m_initialized) {
      TableIdentifier table;
      RangeSpec range;
      RangeServerClient rsc(m_conn_manager_ptr->get_comm());

      HT_INFO("Initializing METADATA");

      /**
       * Create METADATA table
       */
      {
        String metadata_schema_file = System::install_dir
                                      + "/conf/METADATA.xml";
        off_t schemalen;
        const char *schemastr =
          FileUtils::file_to_buffer(metadata_schema_file.c_str(), &schemalen);

        try {
          create_table("METADATA", schemastr);
        }
        catch (Exception &e) {
          if (e.code() != Error::MASTER_TABLE_EXISTS) {
            HT_ERROR_OUT << e << HT_END;
            HT_ABORT;
          }
          exists = true;
        }
      }

      /**
       * Open METADATA table
       */
      m_metadata_table_ptr = new Table(m_props_ptr, m_conn_manager_ptr,
                                       m_hyperspace_ptr, "METADATA");

      /**
       * If table exists, then ranges should already have been assigned,
       * so figure out the location of the root METADATA server, and
       * set the root_server_connected flag appropriately
       */
      if (exists) {
        DynamicBuffer dbuf;
        try {
          HandleCallbackPtr null_callback;
          uint64_t handle = m_hyperspace_ptr->open("/hypertable/root",
              OPEN_FLAG_READ, null_callback);
          m_hyperspace_ptr->attr_get(handle, "Location", dbuf);
          m_hyperspace_ptr->close(handle);
        }
        catch (Exception &e) {
          HT_FATALF("Unable to read '/hypertable/root:Location' in hyperspace "
                    "- %s - %s,", Error::get_text(e.code()), e.what());
        }
        m_root_server_location = (const char *)dbuf.base;
        if (m_root_server_location == location) {
          m_root_server_connected = true;
          m_root_server_addr = cb->get_address();
        }
        m_initialized = true;
        m_root_server_cond.notify_all();
        HT_INFO("METADATA table already exists");
        return;
      }

      m_metadata_table_ptr->get_identifier(&table);
      table.name = "METADATA";

      /**
       * Load root METADATA range
       */
      range.start_row = 0;
      range.end_row = Key::END_ROOT_ROW;

      try {
        RangeState range_state;
        range_state.soft_limit = m_max_range_bytes;
        rsc.load_range(connection, table, range, 0, range_state);
      }
      catch (Exception &e) {
        HT_ERRORF("Problem issuing 'load range' command for %s[..%s] at server "
                  "%s - %s", table.name, range.end_row,
                  location.c_str(), Error::get_text(e.code()));
      }


      /**
       * Write METADATA entry for second-level METADATA range
       */

      TableMutatorPtr mutator_ptr;
      KeySpec key;
      String metadata_key_str;

      mutator_ptr = m_metadata_table_ptr->create_mutator();

      metadata_key_str = String("0:") + Key::END_ROW_MARKER;
      key.row = metadata_key_str.c_str();
      key.row_len = metadata_key_str.length();
      key.column_qualifier = 0;
      key.column_qualifier_len = 0;

      try {
        key.column_family = "StartRow";
        mutator_ptr->set(key, (uint8_t *)Key::END_ROOT_ROW,
                         strlen(Key::END_ROOT_ROW));
        mutator_ptr->flush();
      }
      catch (Hypertable::Exception &e) {
        // TODO: propagate exception
        HT_ERRORF("METADATA update error (row_key = %s) - %s : %s",
                  metadata_key_str.c_str(), e.what(),
                  Error::get_text(e.code()));
        exit(1);
      }

      /**
       * Load second-level METADATA range
       */
      range.start_row = Key::END_ROOT_ROW;
      range.end_row = Key::END_ROW_MARKER;

      try {
        RangeState range_state;
        range_state.soft_limit = m_max_range_bytes;
        rsc.load_range(connection, table, range, 0, range_state);
      }
      catch (Exception &e) {
        HT_ERRORF("Problem issuing 'load range' command for %s[..%s] at server "
                  "%s - %s", table.name, range.end_row,
                  location.c_str(), Error::get_text(e.code()));
      }

      HT_INFO("METADATA table successfully initialized");

      m_root_server_location = location;
      m_root_server_addr = addr;
      m_root_server_connected = true;
      m_initialized = true;
      m_root_server_cond.notify_all();
    }
    else if (!m_root_server_connected && location == m_root_server_location) {
      m_root_server_connected = true;
      m_root_server_addr = addr;
      m_root_server_cond.notify_all();
    }
  }

}

/**
 * TEMPORARY: Just turns around and assigns new range to caller
 *
 * NOTE: this call can't be protected by a mutex because it can cause the
 * whole system to wedge under certain situations
 */
void
Master::report_split(ResponseCallback *cb, const TableIdentifier &table,
    const RangeSpec &range, const char *transfer_log_dir, uint64_t soft_limit) {
  RangeServerClient rsc(m_conn_manager_ptr->get_comm());
  QualifiedRangeSpec fqr_spec(table, range);
  bool server_pinned = false;
  String location;
  CommAddress addr;

  HT_INFOF("Entering report_split for %s[%s:%s].", table.name, range.start_row,
           range.end_row);

  wait_for_root_metadata_server();

  {
    ScopedLock lock(m_mutex);
    RangeToLocationMap::iterator iter = m_range_to_location_map.find(fqr_spec);
    if (iter != m_range_to_location_map.end()) {
      location = (*iter).second;
      RangeServerStateMap::iterator smiter = m_server_map.find(location);
      if (smiter == m_server_map.end() || !(*smiter).second->connected) {
        cb->error(Error::COMM_NOT_CONNECTED, location);
        return;
      }
      addr = (*smiter).second->addr;
      HT_INFOF("Re-attempting to assign newly reported range %s[%s:%s] to %s",
               table.name, range.start_row, range.end_row, location.c_str());
      server_pinned = true;
    }
    else {
      if (m_server_map_iter == m_server_map.end())
        m_server_map_iter = m_server_map.begin();
      assert(m_server_map_iter != m_server_map.end());
      location = (*m_server_map_iter).second->location;
      addr = (*m_server_map_iter).second->addr;
      HT_INFOF("Assigning newly reported range %s[%s:%s] to %s",
               table.name, range.start_row, range.end_row, location.c_str());
      ++m_server_map_iter;
    }
  }


  try {
    RangeState range_state;
    range_state.soft_limit = soft_limit;
    rsc.load_range(addr, table, range, transfer_log_dir, range_state);
    HT_INFOF("report_split for %s[%s:%s] successful.", table.name,
             range.start_row, range.end_row);
  }
  catch (Exception &e) {
    if (e.code() == Error::RANGESERVER_RANGE_ALREADY_LOADED) {
      HT_ERROR_OUT << e << HT_END;
      {
	ScopedLock lock(m_mutex);
	m_range_to_location_map.erase(fqr_spec);
      }
      cb->response_ok();
    }
    else {
      HT_ERRORF("Problem issuing 'load range' command for %s[%s:%s] at server "
                "%s - %s", table.name, range.start_row, range.end_row,
                location.c_str(), Error::get_text(e.code()));
      {
	ScopedLock lock(m_mutex);
	if (!server_pinned)
	  m_range_to_location_map[fqr_spec] = location;
      }
      cb->error(e.code(), e.what());
    }
    return;
  }

  if (server_pinned) {
    ScopedLock lock(m_mutex);
    m_range_to_location_map.erase(fqr_spec);
  }

  cb->response_ok();
}

void
Master::drop_table(ResponseCallback *cb, const char *table_name,
                   bool if_exists) {
  int saved_error = Error::OK;
  String err_msg;
  String table_file = (String)"/hypertable/tables/" + table_name;
  DynamicBuffer value_buf(0);
  int ival;
  HandleCallbackPtr null_handle_callback;
  uint64_t handle;
  String table_name_str = table_name;

  HT_INFOF("Entering drop_table for %s", table_name);

  wait_for_root_metadata_server();

  try {

    /**
     * Open table file
     */
    try {
      handle = m_hyperspace_ptr->open(table_file.c_str(), OPEN_FLAG_READ,
                                      null_handle_callback);
    }
    catch (Exception &e) {
      if (if_exists && e.code() == Error::HYPERSPACE_BAD_PATHNAME) {
        cb->response_ok();
        return;
      }
      HT_THROW2F(e.code(), e, "Problem opening file '%s'", table_file.c_str());
    }

    m_hyperspace_ptr->attr_get(handle, "table_id", value_buf);

    m_hyperspace_ptr->close(handle);

    ival = atoi((const char *)value_buf.base);

    {
      char start_row[16];
      char end_row[16];
      TableScannerPtr scanner_ptr;
      ScanSpec scan_spec;
      Cell cell;
      String location_str;
      ConnectionMap connections;
      ConnectionMap::iterator cmiter;
      RangeServerStateMap::iterator smiter;
      TableIdentifier table;
      RowInterval ri;

      table.name = table_name_str.c_str();
      table.id = ival;
      table.generation = 0;

      sprintf(start_row, "%d:", ival);
      sprintf(end_row, "%d:%s", ival, Key::END_ROW_MARKER);

      scan_spec.row_limit = 0;
      scan_spec.max_versions = 1;
      scan_spec.columns.clear();
      scan_spec.columns.push_back("Location");

      ri.start = start_row;
      ri.end = end_row;
      scan_spec.row_intervals.push_back(ri);

      scanner_ptr = m_metadata_table_ptr->create_scanner(scan_spec);

      while (scanner_ptr->next(cell)) {
	location_str = String((const char *)cell.value, cell.value_len);
	boost::trim(location_str);
	if (connections.find(location_str) == connections.end()) {
	  ScopedLock lock(m_mutex);
	  if ((smiter = m_server_map.find(location_str)) == m_server_map.end()) {
	    /** Drop failed clean up & return **/
	    saved_error = Error::RANGESERVER_UNAVAILABLE;
	    err_msg = location_str;
	    HT_ERRORF("DROP TABLE failed '%s' - %s", err_msg.c_str(),
		      Error::get_text(saved_error));
	    break;
	  }
	  connections[location_str] = (*smiter).second->connection;
	}
      }

      if (saved_error != Error::OK) {
	cb->error(saved_error, err_msg);
	return;
      }

      if (!connections.empty()) {
	DispatchHandlerDropTable sync_handler(table, m_conn_manager_ptr->get_comm());
        RangeServerStatePtr state_ptr;

	// Issue DROP TABLE commands to RangeServers
	for (cmiter = connections.begin(); cmiter != connections.end(); ++cmiter) {
	  CommAddress addr;
	  addr.set_proxy((*cmiter).first);
	  sync_handler.add((*cmiter).second);
	}

        if (!sync_handler.wait_for_completion()) {
          std::vector<DispatchHandlerDropTable::ErrorResult> errors;
          sync_handler.get_errors(errors);
          for (size_t i=0; i<errors.size(); i++) {
            HT_WARNF("drop table error - %s - %s", errors[i].msg.c_str(),
                     Error::get_text(errors[i].error));
          }
          cb->error(errors[0].error, errors[0].msg);
          return;
        }
      }

    }

    m_hyperspace_ptr->unlink(table_file.c_str());

    HT_INFOF("DROP TABLE '%s' id=%d success", table_name_str.c_str(), ival);

    cb->response_ok();

  }
  catch (Exception &e) {
    HT_ERROR_OUT << e << HT_END;
    cb->error(e.code(), e.what());
  }
}

  void Master::close(ResponseCallback *cb) {
    RangeServerClient rsc(m_conn_manager_ptr->get_comm());
    std::vector<CommAddress> addresses;

    HT_INFO("CLOSE");

    {
      ScopedLock lock(m_mutex);
      addresses.reserve(m_server_map.size());
      for (RangeServerStateMap::iterator iter = m_server_map.begin();
	   iter != m_server_map.end(); ++iter)
	addresses.push_back((*iter).second->addr);
    }

    for (size_t i=0; i<addresses.size(); i++)
      rsc.close(addresses[i]);

    cb->response_ok();
  }


  void Master::shutdown(ResponseCallback *cb) {
    RangeServerClient rsc(m_conn_manager_ptr->get_comm());
    std::vector<CommAddress> addresses;

    HT_INFO("SHUTDOWN");

    {
      ScopedLock lock(m_mutex);
      addresses.reserve(m_server_map.size());
      for (RangeServerStateMap::iterator iter = m_server_map.begin();
	   iter != m_server_map.end(); ++iter)
	addresses.push_back((*iter).second->addr);
    }

    // issue shutdown commands
    for (size_t i=0; i<addresses.size(); i++)
      rsc.shutdown(addresses[i]);

    {
      ScopedLock lock(m_mutex);
      boost::xtime expire_time;
      boost::xtime_get(&expire_time, boost::TIME_UTC);
      expire_time.sec += (int64_t)30;
      m_no_servers_cond.timed_wait(lock, expire_time);
    }

    int server_map_size;

    {
      ScopedLock lock(m_mutex);
      server_map_size = m_server_map.size();
    }

    if (server_map_size != 0) {
      String err_msg = format("%d RangeServers failed to shutdown",
			      server_map_size);
      cb->error(Error::REQUEST_TIMEOUT, err_msg);
      return;
    }

    m_hyperspace_ptr = 0;

    cb->response_ok();

    poll(0, 0, 1000);

    _exit(0);

  }

  void Master::do_maintenance() {
    get_statistics(true);
  }


void
Master::create_table(const char *tablename, const char *schemastr) {
  String finalschema = "";
  String tablefile = (String)"/hypertable/tables/" + tablename;
  string table_basedir;
  string agdir;
  Schema *schema = 0;
  HandleCallbackPtr null_handle_callback;
  uint64_t handle;
  uint32_t table_id;

  /**
   * Check for table existence
   */
  if (m_hyperspace_ptr->exists(tablefile))
    HT_THROW(Error::MASTER_TABLE_EXISTS, tablename);

  /**
   *  Parse Schema and assign Generation number and Column ids
   */
  schema = Schema::new_instance(schemastr, strlen(schemastr));
  if (!schema->is_valid())
    HT_THROW(Error::MASTER_BAD_SCHEMA, schema->get_error_string());

  schema->assign_ids();
  schema->render(finalschema);
  HT_DEBUG_OUT <<"schema:\n"<< finalschema << HT_END;

  /**
   * Create table file
   */
  handle = m_hyperspace_ptr->open(tablefile,
      OPEN_FLAG_READ|OPEN_FLAG_WRITE|OPEN_FLAG_CREATE, null_handle_callback);

  /**
   * Write 'table_id' attribute of table file and 'last_table_id' attribute of
   * /hypertable/master
   */
  {
    char numbuf[16];
    if (!strcmp(tablename, "METADATA")) {
      table_id = 0;
      sprintf(numbuf, "%u", table_id);
      cout << "table id = " << numbuf << endl;
    }
    else {
      table_id = (uint32_t)atomic_inc_return(&m_last_table_id);
      sprintf(numbuf, "%u", table_id);
      cout << "table id = " << numbuf << endl;
      m_hyperspace_ptr->attr_set(m_master_file_handle, "last_table_id",
                                 numbuf, strlen(numbuf)+1);
    }

    m_hyperspace_ptr->attr_set(handle, "table_id", numbuf, strlen(numbuf)+1);
  }

  /**
   * Write schema attribute
   */
  m_hyperspace_ptr->attr_set(handle, "schema", finalschema.c_str(),
                             finalschema.length());

  m_hyperspace_ptr->close(handle);

  /**
   * Create /hypertable/tables/&lt;table&gt;/&lt;accessGroup&gt; directories
   * for this table in DFS
   */
  table_basedir = (string)"/hypertable/tables/" + tablename + "/";

  foreach(const Schema::AccessGroup *ag, schema->get_access_groups()) {
    agdir = table_basedir + ag->name;
    m_dfs_client->mkdirs(agdir);
  }

  /**
   * Write METADATA entry, single range covering entire table '\\0' to 0xff 0xff
   */
  if (table_id != 0) {
    TableMutatorPtr mutator_ptr;
    KeySpec key;
    String metadata_key_str;
    CommAddress addr;
    String location;

    mutator_ptr = m_metadata_table_ptr->create_mutator();

    metadata_key_str = String("") + table_id + ":" + Key::END_ROW_MARKER;
    key.row = metadata_key_str.c_str();
    key.row_len = metadata_key_str.length();
    key.column_qualifier = 0;
    key.column_qualifier_len = 0;

    key.column_family = "StartRow";
    mutator_ptr->set(key, 0, 0);
    mutator_ptr->flush();

    /**
     * TEMPORARY:  ask the one Range Server that we know about to load the range
     */

    TableIdentifier table;
    RangeSpec range;
    uint64_t soft_limit;
    RangeServerClient rsc(m_conn_manager_ptr->get_comm());

    table.name = tablename;
    table.id = table_id;
    table.generation = schema->get_generation();

    range.start_row = 0;
    range.end_row = Key::END_ROW_MARKER;

    {
      ScopedLock lock(m_mutex);
      if (m_server_map_iter == m_server_map.end())
        m_server_map_iter = m_server_map.begin();
      assert(m_server_map_iter != m_server_map.end());
      addr = (*m_server_map_iter).second->addr;
      location = (*m_server_map_iter).second->location;
      HT_INFOF("Assigning first range %s[:%s] to %s", table.name,
	       range.end_row, location.c_str());
      ++m_server_map_iter;
      soft_limit = m_max_range_bytes / std::min(64, (int)m_server_map.size()*2);
    }

    try {
      RangeState range_state;
      range_state.soft_limit = soft_limit;
      rsc.load_range(addr, table, range, 0, range_state);
    }
    catch (Exception &e) {
      String err_msg = format("Problem issuing 'load range' command for "
          "%s[..%s] at server %s - %s", table.name, range.end_row,
          location.c_str(), Error::get_text(e.code()));
      if (schema != 0)
        delete schema;
      HT_THROW2(e.code(), e, err_msg);
    }
  }

  delete schema;
  if (m_verbose) {
    HT_INFOF("Successfully created table '%s' ID=%d", tablename, table_id);
  }

}

/**
 * PRIVATE Methods
 */

bool Master::initialize() {
  uint64_t handle;
  HandleCallbackPtr null_handle_callback;

  try {

    if (!m_hyperspace_ptr->exists("/hypertable")) {
      if (!create_hyperspace_dir("/hypertable"))
        return false;
    }

    if (!m_hyperspace_ptr->exists("/hypertable/servers")) {
      if (!create_hyperspace_dir("/hypertable/servers"))
        return false;
    }

    if (!m_hyperspace_ptr->exists("/hypertable/tables")) {
      if (!create_hyperspace_dir("/hypertable/tables"))
        return false;
    }

    // Create /hypertable/master if necessary
    handle = m_hyperspace_ptr->open("/hypertable/master",
        OPEN_FLAG_READ|OPEN_FLAG_WRITE|OPEN_FLAG_CREATE, null_handle_callback);
    m_hyperspace_ptr->close(handle);

    /**
     *  Create /hypertable/root
     */
    handle = m_hyperspace_ptr->open("/hypertable/root",
        OPEN_FLAG_READ|OPEN_FLAG_WRITE|OPEN_FLAG_CREATE, null_handle_callback);
    m_hyperspace_ptr->close(handle);

    /**
     * Create dir for storing monitoring stats
     */
    ms_monitoring_dir = System::install_dir + "/run/monitoring/";
    if (!FileUtils::exists(ms_monitoring_dir)) {
      if (!FileUtils::mkdirs(ms_monitoring_dir)) {
        HT_THROW(Error::LOCAL_IO_ERROR, "Unable to create monitoring dir ");
      }
      HT_INFO("Created monitoring stats dir");
    }
    else
      HT_INFO("monitoring stats dir exists");

    HT_INFO("Successfully Initialized Hypertable.");

    return true;

  }
  catch (Exception &e) {
    HT_ERROR_OUT << e << HT_END;
    return false;
  }
}


/**
 *
 */
void Master::scan_servers_directory() {
  ScopedLock lock(m_mutex);
  HandleCallbackPtr lock_file_handler;
  std::vector<struct DirEntry> listing;
  uint32_t lock_status;
  LockSequencer lock_sequencer;
  RangeServerStatePtr rs_state;
  uint32_t oflags;
  String hsfname;
  uint64_t hyperspace_handle;
  std::vector<String> names;

  try {

    /**
     * Open /hyperspace/servers directory and scan for range servers
     */
    m_servers_dir_callback_ptr =
        new ServersDirectoryHandler(this, m_app_queue_ptr);

    m_servers_dir_handle = m_hyperspace_ptr->open("/hypertable/servers",
        OPEN_FLAG_READ, m_servers_dir_callback_ptr);

    m_hyperspace_ptr->attr_list(m_servers_dir_handle, names);
    for (size_t i=0; i<names.size(); i++)
      HT_INFOF("Mapping: %s", names[i].c_str());

    m_hyperspace_ptr->readdir(m_servers_dir_handle, listing);

    oflags = OPEN_FLAG_READ | OPEN_FLAG_WRITE | OPEN_FLAG_LOCK;

    for (size_t i=0; i<listing.size(); i++) {

      rs_state = new RangeServerState();
      rs_state->location = listing[i].name;
      rs_state->addr.set_proxy(listing[i].name);

      hsfname = (String)"/hypertable/servers/" + listing[i].name;

      lock_file_handler =
          new ServerLockFileHandler(rs_state, this, m_app_queue_ptr);

      hyperspace_handle =
          m_hyperspace_ptr->open(hsfname, oflags, lock_file_handler);

      m_hyperspace_ptr->try_lock(hyperspace_handle,
          LOCK_MODE_EXCLUSIVE, &lock_status, &lock_sequencer);

      if (lock_status == LOCK_STATUS_GRANTED) {
        HT_INFOF("Obtained lock on servers file %s, removing...",
                 hsfname.c_str());
        m_hyperspace_ptr->close(hyperspace_handle);
        m_hyperspace_ptr->unlink(hsfname);
      }
      else {
        m_hyperspace_ptr->close(hyperspace_handle);
        m_server_map[rs_state->location] = rs_state;
      }
    }
  }
  catch (Exception &e) {
    HT_ERROR_OUT << e << HT_END;
    HT_ABORT;
  }
}


/**
 *
 */
bool Master::create_hyperspace_dir(const String &dir) {

  try {

    if (m_hyperspace_ptr->exists(dir))
      return true;

    m_hyperspace_ptr->mkdir(dir);

  }
  catch (Exception &e) {
    HT_ERROR_OUT << "Problem creating hyperspace directory '" << dir << "'"
        << HT_END;
    HT_ERROR_OUT << e << HT_END;
    return false;
  }

  return true;
}

bool Master::handle_disconnect(struct sockaddr_in addr, String &location) {
  ScopedLock lock(m_mutex);
  SockAddrMap<String>::iterator iter = m_addr_map.find(addr);
  if (iter == m_addr_map.end())
    return false;
  location = (*iter).second;
  RangeServerStatePtr rs_state = m_server_map[location];
  if (rs_state)
    rs_state->connected = false;
  return true;
}

void Master::join() {
  m_app_queue_ptr->join();
  m_threads.join_all();
}

void Master::wait_for_root_metadata_server() {
  ScopedLock lock(m_root_server_mutex);
  while (!m_root_server_connected) {
    HT_WARN("Waiting for root metadata server ...");
    m_root_server_cond.wait(lock);
  }
}

/**
 *
 */
void
Master::get_statistics(bool snapshot) {

  {
    ScopedLock lock(m_stats_mutex);
    if (m_get_stats_outstanding) {
      HT_WARN_OUT << "get_statistics request outstanding" << HT_END;
      return;
    }
    m_get_stats_outstanding = true;
  }


  wait_for_root_metadata_server();
  String stats_str;

  try {

    TableStatsSnapshotPtr table_stats(new TableStatsSnapshot);
    RangeServerHLStatsSnapshotPtr server_stats(new RangeServerHLStatsSnapshot);

    RangeServerStatsStateMap::iterator state_it;
    RangeServerStatsMap::iterator stats_it;

    // create proxy map and reverse proxy map
    ProxyMapT proxy_map;
    SockAddrMap<String> reverse_proxy_map;
    SockAddrMap<String>::iterator rpm_it;
    ProxyMapT::iterator pm_it;
    {
      ScopedLock lock(m_mutex);
      m_conn_manager_ptr->get_comm()->get_proxy_map(proxy_map);
      RangeServerStateMap::iterator smiter;
      pm_it = proxy_map.begin();
      ProxyMapT::iterator del_it;

      while (pm_it != proxy_map.end()) {
        // This server is not available
        smiter = m_server_map.find(pm_it->first);
        if (smiter == m_server_map.end()) {
          del_it = pm_it;
          ++pm_it;
          proxy_map.erase(del_it);
          continue;
        }
        reverse_proxy_map[smiter->second->connection] = pm_it->first;
        pm_it->second = smiter->second->connection;
        ++pm_it;
      }
    }
    state_it = m_server_stats_state_map.begin();

    // erase any servers that have become unavailable
    while(state_it != m_server_stats_state_map.end()) {
      RangeServerStatsStateMap::iterator del_it;
      if (proxy_map.find(state_it->first) == proxy_map.end()) {
        // blow away stats for now, we'll get full stats when server is available again
        del_it = state_it;
        stats_it = m_server_stats_map.find(del_it->first);
        if (stats_it != m_server_stats_map.end()) {
          delete stats_it->second;
          m_server_stats_map.erase(stats_it);
        }
        ++state_it;
        m_server_stats_state_map.erase(del_it);
      }
      else
        ++state_it;
    }

    // Make room for newly available servers
    pm_it = proxy_map.begin();
    while (pm_it != proxy_map.end()) {
      state_it = m_server_stats_state_map.find(pm_it->first);
      if (state_it == m_server_stats_state_map.end()) {
        m_server_stats_state_map[pm_it->first] = true;
        // TODO: in future allocate this based on the version of stats returned
        m_server_stats_map[pm_it->first] = new RangeServerStatsV0;
      }
      ++pm_it;
    }

    if (!proxy_map.empty()) {
      DispatchHandlerGetStatistics sync_handler(m_conn_manager_ptr->get_comm(), 5000);

      // Issue get_statistics commands to RangeServers
      for (pm_it = proxy_map.begin(); pm_it != proxy_map.end(); ++pm_it) {
        bool all = m_server_stats_state_map[pm_it->first];
        sync_handler.add(pm_it->second, all, snapshot);
      }

      if (!sync_handler.wait_for_completion()) {
        std::vector<DispatchHandlerGetStatistics::ErrorResult> errors;
        sync_handler.get_errors(errors);

        for (size_t ii=0; ii<errors.size(); ii++) {
          String proxy_addr;
          HT_ERROR_OUT << "'get_statistics' call failed on server " << errors[ii].addr.to_str()
                       << HT_END;
          // blow away stats for now, we'll get full stats when server is available again
          if (errors[ii].addr.is_inet()) {
            rpm_it = reverse_proxy_map.find(errors[ii].addr.inet);
            HT_ASSERT(rpm_it != reverse_proxy_map.end());
            proxy_addr = rpm_it->second;
          }
          else
            proxy_addr = errors[ii].addr.proxy;
          m_server_stats_state_map.erase(proxy_addr);
          stats_it = m_server_stats_map.find(proxy_addr);
          if (stats_it != m_server_stats_map.end()) {
            delete stats_it->second;
            m_server_stats_map.erase(stats_it);
          }
        }
      }

      CommAddressMap<EventPtr> responses;
      CommAddressMap<EventPtr>::iterator responses_it;
      sync_handler.get_responses(responses);
      responses_it = responses.begin();
      while (responses_it != responses.end()) {
        String proxy_addr;
        const uint8_t *decode_ptr = responses_it->second->payload + 4;
        size_t decode_remain = responses_it->second->payload_len - 4;
        uint16_t version = decode_i16(&decode_ptr, &decode_remain);
        HT_ASSERT(version==0);
        proxy_addr = decode_vstr(&decode_ptr, &decode_remain);
        stats_it = m_server_stats_map.find(proxy_addr);
        HT_ASSERT(stats_it != m_server_stats_map.end());
        stats_it->second->process_stats(&decode_ptr, &decode_remain, true,
                                        table_stats->map);
        // Uncomment for debugging
        //String str;
        //stats_it->second->dump_str(str);
        //HT_INFO_OUT << "Got statistics " << str << HT_END;

        // insert high level server stats into server_stats
        // range server stats buffer will be responsible for deleting hl_stats
        RangeServerHLStats *hl_stats = new RangeServerHLStats;
        stats_it->second->get_hl_stats(*hl_stats);

        server_stats->map[proxy_addr] = hl_stats;

        // just get stats for changed ranges next time
        m_server_stats_state_map[proxy_addr] = false;
        ++responses_it;

        stats_it->second->dump_rrd(ms_monitoring_dir + stats_it->first);
      }
      // check if table id to name mapping needs refresh
      TableStatsMap::const_iterator iter = (table_stats->map).begin();
      for (; iter != (table_stats->map).end(); ++iter) {
        if (m_table_ids_to_names.find(iter->first) == m_table_ids_to_names.end()) {
          refresh_table_id_mapping();
          break;
        }
      }

      table_stats->timestamp = Hypertable::get_ts64();

      m_table_stats_buffer.push_front(table_stats);
      server_stats->timestamp = table_stats->timestamp;
      m_range_server_stats_buffer.push_front(server_stats);

      // dump stats to text files
      String table_stats_file = ms_monitoring_dir + "table_stats.txt.tmp";
      String range_server_stats_file = ms_monitoring_dir + "rs_stats.txt.tmp";
      filebuf fb_table, fb_range_server;

      if (!fb_table.open(table_stats_file.c_str(), ios::out))
        HT_ERROR_OUT << "Couldn't open " << table_stats_file << "for output" << HT_END;
      else {
        ostream os(&fb_table);
        dump_table_snapshot_buffer(m_table_stats_buffer, m_table_ids_to_names, os);
        fb_table.close();
        Path from(ms_monitoring_dir + "table_stats.txt.tmp");
        Path to(ms_monitoring_dir + "table_stats.txt");
        // have to remove if exists since boost::filesystem doesnt allow atomic move
        boost::filesystem::remove(to);
        boost::filesystem::rename(from, to);
      }
      if (!fb_range_server.open(range_server_stats_file.c_str(), ios::out))
        HT_ERROR_OUT << "Couldn't open " << range_server_stats_file << "for output" << HT_END;
      else {
        ostream os(&fb_range_server);
        dump_range_server_snapshot_buffer(m_range_server_stats_buffer, os);
        fb_range_server.close();
        Path from(ms_monitoring_dir + "rs_stats.txt.tmp");
        Path to(ms_monitoring_dir + "rs_stats.txt");
        // have to remove if exists since boost::filesystem doesnt allow atomic move
        boost::filesystem::remove(to);
        boost::filesystem::rename(from, to);
      }
    }
  }
  catch (Exception &e) {
    HT_ERROR_OUT << e << HT_END;
  }
  catch (std::exception &e) {
    HT_ERRORF("caught std::exception: %s", e.what());
  }

  {
    ScopedLock lock(m_stats_mutex);
    m_get_stats_outstanding = false;
  }

}

/**
 *
 */
void
Master::refresh_table_id_mapping() {
  uint32_t id;
  HandleCallbackPtr null_handle_callback;
  uint64_t handle = m_hyperspace_ptr->open("/hypertable/tables", OPEN_FLAG_READ,
                              null_handle_callback);
  vector<Hyperspace::DirEntryAttr> listing;
  String attr("table_id");

  m_hyperspace_ptr->readdir_attr(handle, attr, listing);
  // blow away existing mappings and repopulate
  m_table_names_to_ids.clear();
  m_table_ids_to_names.clear();
  for (size_t ii=0; ii<listing.size(); ii++) {
    if (!listing[ii].is_dir) {
      id = (uint32_t)atoi((const char*)listing[ii].attr.base);
      m_table_ids_to_names[id] = listing[ii].name;
      m_table_names_to_ids[listing[ii].name] = id;
    }
  }

  m_hyperspace_ptr->close(handle);
}



} // namespace Hypertable
