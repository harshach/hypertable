/**
 * Copyright (C) 2009 Doug Judd (Zvents, Inc.)
 *
 * This file is part of Hypertable.
 *
 * Hypertable is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
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
#include <cstring>

extern "C" {
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
}

#include "AsyncComm/ApplicationQueue.h"

#include "Common/Mutex.h"
#include "Common/Error.h"
#include "Common/Filesystem.h"
#include "Common/FileUtils.h"
#include "Common/StringExt.h"
#include "Common/System.h"
#include "Common/Serialization.h"

#include "DfsBroker/Lib/Client.h"

#include "RequestHandlerRecoverOpen.h"

#include "Config.h"
#include "Event.h"
#include "Notification.h"
#include "Master.h"
#include "Session.h"
#include "SessionData.h"

using namespace Hypertable;
using namespace Hypertable::Config;
using namespace Hyperspace;
using namespace std;

#define HT_BDBTXN_BEGIN(_label_) \
  do { \
    DbTxn *txn = m_bdb_fs->start_transaction(); \
    if (Global::failure_inducer)\
      Global::failure_inducer->maybe_fail(_label_); \
    try

#define HT_BDBTXN_END_CB(_label_, _cb_) \
    catch (Exception &e) { \
      if (e.code() != Error::HYPERSPACE_BERKELEYDB_DEADLOCK) { \
        if (e.code() == Error::HYPERSPACE_BERKELEYDB_ERROR) \
          HT_ERROR_OUT << e << HT_END; \
        else \
          HT_WARNF("%s - %s", Error::get_text(e.code()), e.what()); \
        txn->abort(); \
        _cb_->error(e.code(), e.what()); \
        return; \
      } \
      HT_WARN_OUT << "Berkeley DB deadlock encountered in txn "<< txn << HT_END; \
      txn->abort(); \
      poll(0, 0, (System::rand32() % 3000) + 1); \
      continue; \
    } \
    HT_DEBUG_OUT << "end txn " << txn << HT_END; \
    if (Global::failure_inducer)\
      Global::failure_inducer->maybe_fail(_label_); \
    break; \
  } while (true)

#define HT_BDBTXN_END(_label_, ...) \
    catch (Exception &e) { \
      if (e.code() != Error::HYPERSPACE_BERKELEYDB_DEADLOCK) { \
        if (e.code() == Error::HYPERSPACE_BERKELEYDB_ERROR) \
          HT_ERROR_OUT << e << HT_END; \
        else \
          HT_WARNF("%s - %s", Error::get_text(e.code()), e.what()); \
        txn->abort(); \
        return __VA_ARGS__; \
      } \
      HT_WARN_OUT << "Berkeley DB deadlock encountered in txn "<< txn << HT_END; \
      txn->abort(); \
      poll(0, 0, (System::rand32() % 3000) + 1); \
      continue; \
    } \
    HT_DEBUG_OUT << "end txn " << txn << HT_END; \
    if (Global::failure_inducer)\
      Global::failure_inducer->maybe_fail(_label_); \
    break; \
  } while (true)

/**
 * Sets up the m_base_dir variable to be the absolute path of the root of the
 * Hyperspace directory (with no trailing slash); Locks this root directory to
 * prevent concurrent masters and then reads/increments/writes the 32-bit
 * integer attribute 'generation'; Creates the server Keepalive handler.
 */
Master::Master(ConnectionManagerPtr &conn_mgr, PropertiesPtr &props,
               ServerKeepaliveHandlerPtr &keepalive_handler,
               ApplicationQueuePtr &app_queue_ptr)
  : m_verbose(false), m_next_handle_number(1), m_next_session_id(1),
    m_lease_credit(0) {

  m_verbose = props->get_bool("verbose");
  m_lease_interval = props->get_i32("Hyperspace.Lease.Interval");
  m_keep_alive_interval = props->get_i32("Hyperspace.KeepAlive.Interval");

  Path base_dir(props->get_str("Hyperspace.Master.Dir"));

  if (!base_dir.is_complete())
    base_dir = Path(System::install_dir) / base_dir;

  m_base_dir = base_dir.directory_string();

  HT_INFOF("BerkeleyDB base directory = '%s'", m_base_dir.c_str());
  m_lock_file = m_base_dir + "/lock";

  // Make sure base directory exists, create if it doesn't
  if (!FileUtils::exists(m_base_dir.c_str())) {
    HT_INFOF("Base directory '%s' does not exist, creating...",
	     m_base_dir.c_str());
    if (!FileUtils::mkdirs(m_base_dir.c_str())) {
      HT_ERRORF("Unable to create base directory %s - %s",
                m_base_dir.c_str(), strerror(errno));
      exit(1);
    }
  }

  if (!FileUtils::exists(m_lock_file.c_str())) {
    HT_INFOF("Lock file '%s' does not exist, creating...",
	     m_lock_file.c_str());
    if ((m_lock_fd = ::open(m_lock_file.c_str(), O_RDWR|O_CREAT|O_TRUNC, 0644)) < 0) {
      HT_ERRORF("Unable to create lock file '%s' - %s",
		m_lock_file.c_str(), strerror(errno));
      exit(1);
    }
  }
  else {
    if ((m_lock_fd = ::open(m_lock_file.c_str(), O_RDWR)) < 0) {
      HT_ERRORF("Unable to open lock file '%s' - %s",
		m_lock_file.c_str(), strerror(errno));
      exit(1);
    }
  }

  /**
   * Lock the base directory to prevent concurrent masters
   */
#if defined(__sun__)
  struct flock fl;

  memset(&fl, 0, sizeof fl);

  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;
  fl.l_pid = getpid();

  if (fcntl(m_lock_fd, F_SETLKW, &fl) == -1) {
    if (errno == EWOULDBLOCK) {
      HT_ERRORF("Lock file '%s' is locked by another process.",
                m_lock_file.c_str());
    }
    else {
      HT_ERRORF("Unable to lock file '%s' - %s",
                m_lock_file.c_str(), strerror(errno));
    }
    exit(1);
  }
#else
  if (flock(m_lock_fd, LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK) {
      HT_ERRORF("Lock file '%s' is locked by another process.",
                m_lock_file.c_str());
    }
    else {
      HT_ERRORF("Unable to lock file '%s' - %s",
                m_lock_file.c_str(), strerror(errno));
    }
    exit(1);
  }
#endif

  m_bdb_fs = new BerkeleyDbFilesystem(m_base_dir);
  Event::set_bdb_fs(m_bdb_fs);

  /**
   * Load and increment generation number
   */
  get_generation_number();

  uint16_t port = props->get_i16("Hyperspace.Master.Port");
  InetAddr::initialize(&m_local_addr, INADDR_ANY, port);

  app_queue_ptr = new ApplicationQueue( get_i32("workers") );

  boost::xtime_get(&m_last_tick, boost::TIME_UTC);

  m_keepalive_handler_ptr.reset(
   new ServerKeepaliveHandler(conn_mgr->get_comm(), this, app_queue_ptr));
  keepalive_handler = m_keepalive_handler_ptr;

  /**
   * Recover incomplete requests
   */
  do_recovery();
}


Master::~Master() {
  delete m_bdb_fs;
  ::close(m_lock_fd);
}

/**
 * No need for locks since this is happening in a single thread
 */
void
Master::do_recovery()
{
  vector<uint64_t> sessions;

  HT_BDBTXN_BEGIN("do_recovery-begin-1") {
    m_bdb_fs->get_all_sessions(txn, sessions);
    foreach(uint64_t session, sessions) {
      recover_session(txn, session);
    }
  }
  HT_BDBTXN_END("do_recovery-end-1", );

  // extend lease on all active sessions
  // TODO: XXX: I WAS HERE

  // increment master generation number
}

/**
 * No need for locks since this is happening in a single thread
 */
void
Master::recover_session(DbTxn *txn, uint64_t session_id)
{
  SessionDataPtr session_data;
  sockaddr_in addr;
  String addr_str, name;
  ReqTypeMap requests;
  bool expired = m_bdb_fs->session_expired(txn, session_id);

  // load in session data
  addr_str = m_bdb_fs->get_session_addr(txn, session_id);
  if (!InetAddr::initialize(&addr, addr_str.c_str()))
    HT_THROWF(Error::COMM_SOCKET_ERROR, "Bad address:%s", addr_str.c_str());
  name = m_bdb_fs->get_session_name(txn, session_id);
  session_data = new SessionData(addr, m_lease_interval, session_id);
  session_data->set_name(name);

  // check if session is expired
  if(expired) {
    session_data->expire();
    m_expired_sessions.push_back(session_id);
  }

  // go through all incomplete requests for this session and enqueue their recovery
  get_all_session_req_types(txn, session_id, requests);
  foreach (const ReqTypeMap::value_type &v, requests) {
    recover_request(session_id, v.first, v.second);
  }
  // TODO:: XXX I WAS HERE

  // update session map/heap
  m_session_map[session_id] = session_data;
  m_session_heap.push_back(session_data);
}

void
Master::recover_request(uint64_t session_id, uint64_t req_id, int req_type)
{
  ApplicationHandler *handler=0;

  switch(req_type) {
    case Protocol::COMMAND_OPEN:
      handler = new RequestHandlerRecoverOpen(this, session_id, req_id);
      break;
    case Protocol::COMMAND_CLOSE: //<<< I AM HERE XXX: >>>
      break;
    case Protocol::COMMAND_MKDIR:
      break;
    case Protocol::COMMAND_DELETE:
      break;
    case Protocol::COMMAND_ATTRSET:
      break;
    case Protocol::COMMAND_ATTRDEL:
      break;
    case Protocol::COMMAND_LOCK:
      break;
    case Protocol::COMMAND_RELEASE:
      break;
    default:
      HT_EXPECT(false, Error::PROTOCOL_ERROR);
  }
  m_app_queue_ptr->add(handler);
  // add request completion completion to map

}

/**
 * create_session does the following:
 * > Lock the session expiry map
 * > get and incr next session id from BDB
 * > create new session in BDB
 * > insert session id & expiry time into session expiry map
 */
uint64_t Master::create_session(struct sockaddr_in &addr) {
  ScopedLock lock(m_session_map_mutex);

  SessionDataPtr session_data;
  uint64_t session_id = 0;
  String addr_str = InetAddr::format(addr);

  HT_BDBTXN_BEGIN("create_session-begin") {
    // DB updates
    session_id = m_bdb_fs->get_next_id_i64(txn, SESSION_ID, true);
    m_bdb_fs->create_session(txn, session_id, addr_str);
    // in mem updates
    session_data = new SessionData(addr, m_lease_interval, session_id);
    m_session_map[session_id] = session_data;
    m_session_heap.push_back(session_data);

    txn->commit(0);
    HT_INFOF("created session %llu", (Llu)session_id);
  }
  HT_BDBTXN_END("create_session-end", 0);

  return session_id;
}

/**
 *
 */
bool Master::get_session(uint64_t session_id, SessionDataPtr &session_data) {
  ScopedLock lock(m_session_map_mutex);
  SessionMap::iterator iter = m_session_map.find(session_id);
  if (iter == m_session_map.end())
    return false;
  session_data = (*iter).second;
  return true;
}

/**
 * destroy_session does the following:
 * > Lock the session expiry map and erase the session data object from it
 * > Set the expiry time to now
 */
void Master::destroy_session(uint64_t session_id) {
  ScopedLock lock(m_session_map_mutex);
  SessionDataPtr session_data;
  SessionMap::iterator iter = m_session_map.find(session_id);
  if (iter == m_session_map.end())
    return;
  session_data = (*iter).second;
  m_session_map.erase(session_id);
  session_data->expire();
  // force it to top of expiration heap
  boost::xtime_get(&session_data->expire_time, boost::TIME_UTC);
  HT_INFOF("destroyed session %llu(%s)",
          (Llu)session_id, session_data->get_name());
}

/**
 *
 */
void Master::initialize_session(uint64_t session_id, const String &name) {
  SessionDataPtr session_data;
  {
    ScopedLock lock(m_session_map_mutex);
    SessionMap::iterator iter = m_session_map.find(session_id);
    if (iter == m_session_map.end()) {
      HT_ERRORF("Unable to initialize session %llu (%s)", (Llu)session_id, name.c_str());
      return;
    }
    session_data = (*iter).second;
  }

  // set session name in BDB and mem
  HT_BDBTXN_BEGIN("initialize_session-begin") {
    m_bdb_fs->set_session_name(txn, session_id, name);
    txn->commit(0);
    session_data->set_name(name);
  }
  HT_BDBTXN_END("initialize_session-end", );

  HT_INFOF("Initialized session %llu (%s)", (Llu)session_id, name.c_str());
}

/**
 * renew_session_lease does the following:
 * > Lock the session expiry map
 * > If session lease can't be renewed
 *   > Do BDB txn to mark session as expired
 *   > Mark in mem session data as expired
 *   > (Don't delete session completely as handles etc need to be cleaned up)
 */
int Master::renew_session_lease(uint64_t session_id, uint64_t oldest_outstanding_req) {
  ScopedLock lock(m_session_map_mutex);
  bool renewed = false;
  bool commited = false;
  SessionDataPtr session_data;

  SessionMap::iterator iter = m_session_map.find(session_id);
  if (iter == m_session_map.end())
    return Error::HYPERSPACE_EXPIRED_SESSION;

  session_data = iter->second;
  renewed = session_data->renew_lease();

  if (!renewed) {
    // if renew failed then delete from BDB
    HT_BDBTXN_BEGIN("renew_session-begin-1") {
      m_bdb_fs->expire_session(txn, session_id);
      txn->commit(0);
      commited = true;
    }
    HT_BDBTXN_END("renew_session-end-1", Error::HYPERSPACE_EXPIRED_SESSION);

    // Do this outside BDB txn since delete event notifications might cause a BDB txn too
    if (commited)
      session_data->expire();

    // in mem session data will be cleaned up from map & heap in remove expired sessions
    return Error::HYPERSPACE_EXPIRED_SESSION;
  }
  else {
    // if theres a new oldest outstanding req then delete complete requests
    if (oldest_outstanding_req != session_data->get_oldest_outstanding_req()) {
      HT_BDBTXN_BEGIN("renew_session-begin-2") {
        m_bdb_fs->delete_lesser_session_reqs(txn, session_id, oldest_outstanding_req);
        txn->commit(0);
      }
      HT_BDBTXN_END("renew_session-end-2", Error::OK);
      session_data->set_oldest_outstanding_req(oldest_outstanding_req);
    }
  }

  return Error::OK;
}

/**
 * next_expired_session does the following:
 * > Lock the session map mutex
 * > Remake the session heap with the session with earliest expiry time on top
 * > If top of heap session is expired
 *   > Pop it from the heap, delete it from the session map
 *   > return true with session data parameter pointing to this session
 * > else return false
 */
bool
Master::next_expired_session(SessionDataPtr &session_data, boost::xtime &now) {
  ScopedLock lock(m_session_map_mutex);
  struct LtSessionData ascending;

  if (m_session_heap.size() > 0) {
    std::make_heap(m_session_heap.begin(), m_session_heap.end(), ascending);
    if (m_session_heap[0]->is_expired(now)) {
      session_data = m_session_heap[0];
      std::pop_heap(m_session_heap.begin(), m_session_heap.end(), ascending);
      m_session_heap.resize(m_session_heap.size()-1);
      m_session_map.erase(session_data->get_id());
      return true;
    }
  }
  return false;
}


/**
 * remove_expired_sessions does the following:
 * > extend session expiry in case of suspension
 * > mark all expired sessions & get their open handles
 * > destroy all expired & open handles
 * > delete expired sessions in BDB
 */
void Master::remove_expired_sessions(bool recover) {
  SessionDataPtr session_data;
  int error;
  std::string errmsg;
  std::vector<uint64_t> handles;
  std::vector<uint64_t> expired_sessions;
  boost::xtime now;
  uint64_t lease_credit;
  ScopedLock lock(m_remove_expired_sessions_mutex);

  if (!recover) {
    // start extend expiry in case of suspension
    {
      ScopedLock lock(m_last_tick_mutex);
      lease_credit = m_lease_credit;
    }

    boost::xtime_get(&now, boost::TIME_UTC);

    // try recomputing lease credit
    if (lease_credit == 0) {
      lease_credit = xtime_diff_millis(m_last_tick, now);
      if (lease_credit < 5000)
        lease_credit = 0;
    }

    // extend all leases in case of suspension
    if (lease_credit) {
      ScopedLock lock(m_session_map_mutex);
      HT_INFOF("Suspension detected, extending all session leases "
               "by %lu milliseconds", (Lu)lease_credit);
      for (SessionMap::iterator iter = m_session_map.begin();
           iter != m_session_map.end(); iter++)
        (*iter).second->extend_lease((uint32_t)lease_credit);
    }
  } // end extend expiry in case of suspension

  // mark expired sessions
  if (!recover) {
    while (next_expired_session(session_data, now)) {
      bool commited = false;
      if (m_verbose)
        HT_INFOF("Expiring session %llu name=%s", (Llu)session_data->get_id(),
                 session_data->get_name());
      commited = false;
      // expire session_data in mem and in BDB
      HT_BDBTXN_BEGIN("remove_expired_sessions-begin-1") {
        m_bdb_fs->get_session_handles(txn, session_data->get_id(), handles);
        m_bdb_fs->expire_session(txn, session_data->get_id());
        txn->commit(0);
        commited = true;
        expired_sessions.push_back(session_data->get_id());
      }
      HT_BDBTXN_END("remove_expired_sessions-end-1", );
      // keep this outside the BDB txn since
      if (commited)
        session_data->expire();
    }
  }
  else {
    expired_sessions = m_expired_sessions;
    // during recovery get all open handles for expired sessions
    HT_BDBTXN_BEGIN("remove_expired_sessions-begin-2") {
      foreach (uint64_t expired_session, expired_sessions) {
        m_bdb_fs->get_session_handles(txn, expired_session, handles);
      }
      txn->commit(0);
    }
    HT_BDBTXN_END("remove_expired_sessions-end-2", );
  }

  // delete handles open by expired sessions
  foreach(uint64_t handle, handles) {
    if (m_verbose)
      HT_INFOF("Destroying handle %llu", (Llu)handle);
    if (!destroy_handle(handle, &error, errmsg, false, recover))
      HT_ERRORF("Problem destroying handle - %s (%s)",
                Error::get_text(error), errmsg.c_str());
  }

  // delete expired sessions from BDB
  if (expired_sessions.size() > 0) {
    HT_BDBTXN_BEGIN("remove_expired_sessions-begin-3") {
      foreach (uint64_t expired_session, expired_sessions) {
        m_bdb_fs->delete_session(txn, expired_session);
      }
      txn->commit(0);
    }
    HT_BDBTXN_END("remove_expired_sessions-end-3", );
  }

  if (recover)
    m_expired_sessions.clear();
}

/**
 * Creates a directory with absolute path 'name'.
 *
 * Does the following:
 * > Find parent node
 * > Start BerkeleyDB txn to verify (and thus prevent modifications to) parent node,
 *     create new dir and
 * > Send out CHILD_NODE_ADDED notifications
 */
void
Master::mkdir(ResponseCallback *cb, uint64_t session_id, uint64_t req_id, const char *name) {
  String abs_name, parent_node;
  String child_name;
  uint64_t event_id;
  HyperspaceEventPtr child_added_event;
  NotificationMap child_added_notifications;
  bool persisted_notifications = false;
  bool commited = false, aborted=false;
  int error=0;
  String error_msg;
  ReqInfo req_info;

  if (m_verbose) {
    HT_INFOF("mkdir(session_id=%llu, name=%s)", (Llu)session_id, name);
  }

  if (!find_parent_node(name, parent_node, child_name)) {
    cb->error(Error::HYPERSPACE_FILE_EXISTS, "directory '/' exists");
    return;
  }

  assert(name[0] == '/' && name[strlen(name)-1] != '/');

  HT_BDBTXN_BEGIN("mkdir-begin") {
    // make sure parent node data is setup
    if (!validate_and_create_node_data(txn, parent_node)) {
      error = Error::HYPERSPACE_FILE_NOT_FOUND;
      error_msg = (String)"' parent node: '" + parent_node + "'";
      aborted = true;
      goto txn_commit;
    }

    // create event and persist notifications
    event_id = m_bdb_fs->get_next_id_i64(txn, EVENT_ID, true);
    m_bdb_fs->create_event(txn, EVENT_TYPE_NAMED, event_id, EVENT_MASK_CHILD_NODE_ADDED,
                           child_name);
    child_added_event = new EventNamed(event_id, EVENT_MASK_CHILD_NODE_ADDED, child_name);
    if (m_bdb_fs->get_node_event_notification_map(txn, parent_node,
        EVENT_MASK_CHILD_NODE_ADDED, child_added_notifications)) {
      persist_event_notifications(txn, event_id, child_added_notifications);
      persisted_notifications = true;
      req_info.event_notifications.push_back(event_id);
    }
    // create node
    m_bdb_fs->mkdir(txn, name);

    // create node data
    m_bdb_fs->create_node(txn, name, false, 1);

    // store req info
    req_info.type = Protocol::COMMAND_MKDIR;
    req_info.state = 1;
    m_bdb_fs->add_session_req(txn, session_id, req_id, req_info);

    txn_commit:
    if (aborted)
      txn->abort();
    else {
      txn->commit(0);
      commited = true;
    }
  }
  HT_BDBTXN_END_CB("mkdir-end", cb);

  // check for errors
  if (aborted) {
    cb->error(error, error_msg);
    HT_ERROR_OUT << Error::get_text(error) << " - " << error_msg << HT_END;
    return;
  }

  // Deliver notifications if needed
  if (commited && persisted_notifications) {
    deliver_event_notifications(child_added_event, child_added_notifications);
  }

  cb->response_ok();
}


/**
 * Unlink
 *
 * Does the following:
 *
 * > Get the parent node
 * > Start BDB txn and validate (and so lock) the parent node
 * > Validate node to be deleted
 * > If there are no handles open for this node, create and persist node removed notifications
 * > Delete node from BDB fs and delete node data from BDB
 * > End BDB txn
 * > Deliver notifications
 */
void
Master::unlink(ResponseCallback *cb, uint64_t session_id, uint64_t req_id, const char *name) {
  String child_name, parent_node;
  String node=name;
  bool has_refs;
  uint64_t event_id;
  HyperspaceEventPtr child_removed_event;
  NotificationMap child_removed_notifications;
  bool persisted_notifications = false;
  bool aborted = false;
  bool commited = false;
  String error_msg;
  int error = 0;

  if (m_verbose) {
    HT_INFOF("unlink(session_id=%llu, name=%s)", (Llu)session_id, name);
  }

  if (!strcmp(name, "/")) {
    cb->error(Error::HYPERSPACE_PERMISSION_DENIED,
              "Cannot remove '/' directory");
    return;
  }

  if (!find_parent_node(node, parent_node, child_name)) {
    cb->error(Error::HYPERSPACE_BAD_PATHNAME, name);
    return;
  }

  HT_BDBTXN_BEGIN("unlink-begin") {
      ReqInfo req_info;

    // make sure parent node data is setup
    if (!validate_and_create_node_data(txn, parent_node)) {
      error = Error::HYPERSPACE_FILE_NOT_FOUND;
      error_msg = (String)" node: '" + node;
      aborted = true;
      goto txn_commit;
    }

    // make sure file & node data exist
    if (!validate_and_create_node_data(txn, node)) {
      error = Error::HYPERSPACE_FILE_NOT_FOUND;
      error_msg = node;
      aborted = true;
      goto txn_commit;
    }

    has_refs = m_bdb_fs->node_has_open_handles(txn, node);

    if (has_refs) {
      error = Error::HYPERSPACE_FILE_OPEN;
      error_msg = "File is still open and referred to by some handle";
      aborted = true;
      goto txn_commit;
    }
    // Sanity check
    assert(name[0] == '/' && name[strlen(name)-1] != '/');

    // Create event and persist notifications
    event_id = m_bdb_fs->get_next_id_i64(txn, EVENT_ID, true);
    m_bdb_fs->create_event(txn, EVENT_TYPE_NAMED, event_id, EVENT_MASK_CHILD_NODE_REMOVED,
                           child_name);
    child_removed_event = new EventNamed(event_id, EVENT_MASK_CHILD_NODE_REMOVED, child_name);
    if (m_bdb_fs->get_node_event_notification_map(txn, parent_node,
        EVENT_MASK_CHILD_NODE_REMOVED, child_removed_notifications)) {
      persist_event_notifications(txn, event_id, child_removed_notifications);
      persisted_notifications = true;
      req_info.event_notifications.push_back(event_id);
    }

    // Delete node
    m_bdb_fs->unlink(txn, name);

    // Delete node data
    m_bdb_fs->delete_node(txn, node);

    // store req info
    req_info.type = Protocol::COMMAND_DELETE;
    req_info.state = 1;
    m_bdb_fs->add_session_req(txn, session_id, req_id, req_info);

    txn_commit:
      if (aborted)
        txn->abort();
      else {
        txn->commit(0);
        commited = true;
      }
  }
  HT_BDBTXN_END_CB("unlink-end", cb);

  // check for errors
  if (aborted) {
    cb->error(error, error_msg);
    return;
  }

  // Deliver notifications if needed
  if (commited && persisted_notifications) {
    deliver_event_notifications(child_removed_event, child_removed_notifications);
  }
  cb->response_ok();
}

/**
 * Open
 *
 */
void
Master::open(ResponseCallbackOpen *cb, uint64_t session_id, uint64_t req_id, const char *name,
    uint32_t flags, uint32_t event_mask, std::vector<Attribute> &init_attrs, bool recover,
    bool retry) {

  SessionDataPtr session_data;
  String child_name, node = name, parent_node;
  bool created = false;
  bool is_dir = false;
  bool existed;
  uint64_t handle = 0;
  bool lock_notify = false;
  uint32_t cur_lock_mode = 0;
  uint32_t lock_mode = 0;
  uint64_t lock_generation = 0;
  bool aborted=false, commited=false;
  String error_msg = "";
  int error = 0;
  uint64_t child_added_event_id;
  HyperspaceEventPtr child_added_event;
  NotificationMap child_added_notifications;
  bool persisted_child_added_notifications = false;
  uint64_t lock_acquired_event_id;
  HyperspaceEventPtr lock_acquired_event;
  NotificationMap lock_acquired_notifications;
  bool persisted_lock_acquired_notifications = false;

  assert(name[0] == '/');

  if (!get_session(session_id, session_data))
    HT_THROWF(Error::HYPERSPACE_EXPIRED_SESSION, "%llu", (Llu)session_id);

  if (m_verbose) {
    HT_INFO_OUT << "open(session_id=" << session_id << ", session_name = "
                << session_data->get_name() << ", fname="<< name ", flags=0x"
                << std::hex << flags << ", event_mask=0x" << std::hex << event_mask
                << ", recover=", << recover << ", retry=" << retry << ")" << HT_END;
  }

  if (!find_parent_node(name, parent_node, child_name))
    HT_THROW(Error::HYPERSPACE_BAD_PATHNAME, name);

  if (!init_attrs.empty() && !(flags & OPEN_FLAG_CREATE))
    HT_THROW(Error::HYPERSPACE_CREATE_FAILED,
             "initial attributes can only be supplied on CREATE");

  // TODO: XXX: I AM HERE
  if (recover) {
  }
  if (retry) {
  }
  HT_BDBTXN_BEGIN("open-begin") {
    // initialize vars since this runs in a loop
    aborted = false; commited = false; created = false; is_dir = false; lock_notify = false;
    persisted_lock_acquired_notifications = false; persisted_child_added_notifications = false;
    lock_mode = 0; cur_lock_mode = 0; lock_generation = 0;
    ReqInfo req_info;
    uint32_t ret_val_size = 2*sizeof(uint64_t) + 1;
    uint8_t *ptr, *ret_val;

    // make sure parent node is valid and create node_data if needed
    if (!validate_and_create_node_data(txn, parent_node)) {
      error = Error::HYPERSPACE_FILE_NOT_FOUND;
      error_msg = (String)" node: '" + node + "' parent node: '" + parent_node + "'";
      aborted = true;
      goto txn_commit;
    }

    existed = m_bdb_fs->exists(txn, name, &is_dir);
    if (existed) { // node exists in DB already
      // check flags
      if ((flags & OPEN_FLAG_CREATE) && (flags & OPEN_FLAG_EXCL)) {
        error = Error::HYPERSPACE_FILE_EXISTS;
        error_msg = (String)"mode=CREATE|EXCL";
        aborted = true;
        goto txn_commit;
      }

      if ((flags & OPEN_FLAG_TEMP)) {
        error = Error::HYPERSPACE_FILE_EXISTS;
        error_msg = (String) "Unable to open TEMP file " + node + "because it already exists";
        aborted = true;
        goto txn_commit;
      }

      // create node data if it doesn't exist and set lock generation
      validate_and_create_node_data(txn, node);

      // check for lock mode conflicts
      cur_lock_mode = m_bdb_fs->get_node_cur_lock_mode(txn, node);
      if ((flags & OPEN_FLAG_LOCK_SHARED) == OPEN_FLAG_LOCK_SHARED) {
        if (cur_lock_mode == LOCK_MODE_EXCLUSIVE) {
          error = Error::HYPERSPACE_LOCK_CONFLICT;
          error_msg = node + "";
          aborted = true;
          goto txn_commit;
        }
        lock_mode = LOCK_MODE_SHARED;
        if (!m_bdb_fs->node_has_shared_lock_handles(txn, node))
          lock_notify = true;
      }
      else if ((flags & OPEN_FLAG_LOCK_EXCLUSIVE) == OPEN_FLAG_LOCK_EXCLUSIVE) {
        if (cur_lock_mode == LOCK_MODE_SHARED || cur_lock_mode == LOCK_MODE_EXCLUSIVE) {
          error = Error::HYPERSPACE_LOCK_CONFLICT;
          error_msg = node + "";
          aborted = true;
          goto txn_commit;
        }
        lock_mode = LOCK_MODE_EXCLUSIVE;
        lock_notify = true;
      }
    } // node exists in DB already
    else { // node doesn't exist in DB
      if (!(flags & OPEN_FLAG_CREATE)) {
        error = Error::HYPERSPACE_BAD_PATHNAME;
        error_msg = name;
        aborted = true;
        goto txn_commit;
      }
      // create new node
      lock_generation = 1;
      m_bdb_fs->create(txn, name, (flags & OPEN_FLAG_TEMP));
      m_bdb_fs->set_xattr_i64(txn, name, "lock.generation",
                              lock_generation);

      // create a new node data object in hyperspace
      m_bdb_fs->create_node(txn, node, (flags & OPEN_FLAG_TEMP), lock_generation);

      // Set the initial attributes
      for (size_t i=0; i<init_attrs.size(); i++)
        m_bdb_fs->set_xattr(txn, name, init_attrs[i].name,
                            init_attrs[i].value, init_attrs[i].value_len);
      created = true;
    } // node doesn't exist in DB
    handle = m_bdb_fs->get_next_id_i64(txn, HANDLE_ID, true);
    m_bdb_fs->create_handle(txn, handle, node, flags, event_mask, session_id, false,
                            HANDLE_NOT_DEL);
    m_bdb_fs->add_session_handle(txn, session_id, handle);

    // create node added event and persist notifications
    child_added_event_id = m_bdb_fs->get_next_id_i64(txn, EVENT_ID, true);
    m_bdb_fs->create_event(txn, EVENT_TYPE_NAMED, child_added_event_id,
                           EVENT_MASK_CHILD_NODE_ADDED, child_name);
    child_added_event = new EventNamed(child_added_event_id, EVENT_MASK_CHILD_NODE_ADDED,
                                       child_name);
    if (m_bdb_fs->get_node_event_notification_map(txn, parent_node,
        EVENT_MASK_CHILD_NODE_ADDED, child_added_notifications)) {
      persist_event_notifications(txn, child_added_event_id, child_added_notifications);
      persisted_child_added_notifications = true;
      req_info.event_notifications.push_back(child_added_event_id);
    }

    /**
     * If open flags LOCK_SHARED or LOCK_EXCLUSIVE, then obtain lock
     */
    if (lock_mode != 0) {
      lock_generation = m_bdb_fs->incr_node_lock_generation(txn, node);
      m_bdb_fs->set_xattr_i64(txn, name, "lock.generation",
                              lock_generation);

      m_bdb_fs->set_node_cur_lock_mode(txn, node, lock_mode);
      lock_handle(txn, handle, lock_mode, node);

      // create and persist lock acquired event
      // deliver notification to handles to this same node
      if (lock_notify) {
        lock_acquired_event_id = m_bdb_fs->get_next_id_i64(txn, EVENT_ID, true);

        m_bdb_fs->create_event(txn, EVENT_TYPE_LOCK_ACQUIRED, lock_acquired_event_id,
                               EVENT_MASK_LOCK_ACQUIRED, lock_mode);
        lock_acquired_event = new EventLockAcquired(lock_acquired_event_id, lock_mode);

        if (m_bdb_fs->get_node_event_notification_map(txn, node, EVENT_MASK_LOCK_ACQUIRED,
                                                  lock_acquired_notifications)) {
          persist_event_notifications(txn, lock_acquired_event_id,
                                      lock_acquired_notifications);
          persisted_lock_acquired_notifications = true;
          req_info.event_notifications.push_back(lock_acquired_event_id);
        }
      }
    }

    m_bdb_fs->add_node_handle(txn, node, handle);

    // store req info
    req_info.type = Protocol::COMMAND_OPEN;
    req_info.state = 1;
    ptr = ret_val = new uint8_t[ret_val_size];
    Serialization::encode_i64(&ptr, handle);
    Serialization::encode_i64(&ptr, lock_generation);
    Serialization::encode_bool(&ptr, created);
    req_info.ret_val.set(ret_val, ret_val_size, true);
    m_bdb_fs->add_session_req(txn, session_id, req_id, req_info);

    HT_INFOF("handle %llu created ('%s', session=%llu(%s), flags=0x%x, mask=0x%x)",
             (Llu)handle, node.c_str(), (Llu)session_id, session_data->get_name(),
             flags, event_mask);

    txn_commit:
      if (aborted)
        txn->abort();
      else {
        txn->commit(0);
        commited = true;
      }
  }
  HT_BDBTXN_END_CB("open-end", cb);

  // check for errors
  if (aborted) {
    HT_THROW(error, error_msg);
  }

  // deliver persisted notifications
  if (commited) {
    if (persisted_child_added_notifications) {
      deliver_event_notifications(child_added_event, child_added_notifications);
    }
    if (persisted_lock_acquired_notifications) {
      deliver_event_notifications(lock_acquired_event, lock_acquired_notifications);
    }
  }

  if (m_verbose) {
    HT_INFOF("exitting open(session_id=%llu, session_name = %s, fname=%s, flags=0x%x, event_mask=0x%x)",
        (Llu)session_id, session_data->get_name(), name, flags, event_mask);
  }

  cb->response(handle, created, lock_generation);
}

/**
 * Close
 */
void Master::close(ResponseCallback *cb, uint64_t session_id, uint64_t req_id, uint64_t handle)
{
  SessionDataPtr session_data;
  int error;
  std::string errmsg;
  uint8_t *args, *ptr;
  uint32_t args_size = sizeof(uint64_t);
  ReqInfo req_info;

  if (!get_session(session_id, session_data)) {
    cb->error(Error::HYPERSPACE_EXPIRED_SESSION, "");
    return;
  }

  if (m_verbose) {
    HT_INFOF("close(session=%llu(%s), handle=%llu)", (Llu)session_id,
             session_data->get_name(), (Llu)handle);
  }

  // delete handle from set of open session handles
  HT_BDBTXN_BEGIN("close-begin") {
    m_bdb_fs->delete_session_handle(txn, session_id, handle);
    m_bdb_fs->set_handle_del_state(txn, handle, HANDLE_MARKED_FOR_DEL);
    // store req info
    req_info.type = Protocol::COMMAND_CLOSE;
    req_info.state = 1;
    ptr = args = new uint8_t[args_size];
    Serialization::encode_i64(&ptr, handle);
    req_info.args.set(args, args_size, true);
    m_bdb_fs->add_session_req(txn, session_id, req_id, req_info);

    txn->commit(0);
  }
  HT_BDBTXN_END_CB("close-end", cb);

  // if handle was open then destroy it (release lock if any, grant next
  // pending lock, delete ephemeral etc.)
  if (!destroy_handle(handle, &error, errmsg, true, false, session_id, req_id, req_info.state)) {
    cb->error(error, errmsg);
    return;
  }

  if ((error = cb->response_ok()) != Error::OK) {
    HT_ERRORF("Problem sending back response - %s", Error::get_text(error));
  }
}

/**
 * attr_set does the following:
 *
 * > Make sure session is valid
 * > Make sure handle is valid
 * > Start BDB txn
 *   > Lock node data
 *   > Set attribute in BDB
 *   > Persist ATTR_SET event notifications
 * > End BDB txn
 * > Deliver ATTR_SET notifications
 * > Send response
 */
void
Master::attr_set(ResponseCallback *cb, uint64_t session_id, uint64_t req_id, uint64_t handle,
                 const char *name, const void *value, size_t value_len) {
  SessionDataPtr session_data;
  int error = 0;
  String error_msg;
  String node;
  uint64_t event_id;
  HyperspaceEventPtr attr_set_event;
  NotificationMap attr_set_notifications;
  bool persisted_notifications = false;
  bool aborted = false, commited = false;


  if (!get_session(session_id, session_data))
    HT_THROWF(Error::HYPERSPACE_EXPIRED_SESSION, "%llu", (Llu)session_id);

  if (m_verbose) {
    HT_INFOF("attrset(session=%llu(%s), handle=%llu, name=%s, value_len=%d)",
             (Llu)session_id, session_data->get_name(), (Llu)handle, name, (int)value_len);
  }

  HT_BDBTXN_BEGIN("attr_set-begin") {
    //(re) initialize vars
    persisted_notifications = false; aborted = false; commited = false;
    ReqInfo req_info;

    if (!m_bdb_fs->handle_exists(txn, handle)) {
      error = Error::HYPERSPACE_INVALID_HANDLE;
      error_msg = (String) "handle=" + handle;
      aborted = true;
      goto txn_commit;
    }

    m_bdb_fs->get_handle_node(txn, handle, node);
    m_bdb_fs->set_xattr(txn, node, name, value, value_len);

    // create event notification and persist
    event_id = m_bdb_fs->get_next_id_i64(txn, EVENT_ID, true);
    m_bdb_fs->create_event(txn, EVENT_TYPE_NAMED, event_id, EVENT_MASK_ATTR_SET, name);
    attr_set_event = new EventNamed(event_id, EVENT_MASK_ATTR_SET, name);

    if (m_bdb_fs->get_node_event_notification_map(txn, node, EVENT_MASK_ATTR_SET,
                                                  attr_set_notifications)) {
      persist_event_notifications(txn, event_id, attr_set_notifications);
      persisted_notifications = true;
      req_info.event_notifications.push_back(event_id);
    }
    // store req info
    req_info.type = Protocol::COMMAND_ATTRSET;
    req_info.state = 1;
    m_bdb_fs->add_session_req(txn, session_id, req_id, req_info);

    txn_commit:
      if (aborted)
        txn->abort();
      else {
        txn->commit(0);
        commited = true;
      }
  }
  HT_BDBTXN_END_CB("attr_set-end", cb);

  // check for errors
  if (aborted) {
    HT_THROW(error, error_msg);
  }

  // deliver notifications
  if (commited && persisted_notifications) {
    deliver_event_notifications(attr_set_event, attr_set_notifications);
  }

  if ((error = cb->response_ok()) != Error::OK)
    HT_ERRORF("Problem sending back response - %s", Error::get_text(error));

  if (m_verbose) {
    HT_INFOF("exitting attrset(session=%llu(%s), handle=%llu, name=%s, value_len=%d)",
             (Llu)session_id, session_data->get_name(), (Llu)handle, name, (int)value_len);
  }

}

/**
 * attr_get does the following:
 *
 * > Make sure session is valid
 * > Make sure handle is valid
 * > Start BDB txn
 *   > Lock node data
 *   > Get attribute in BDB
 * > End BDB txn
 * > Send attr value back in response
 *
 */
void
Master::attr_get(ResponseCallbackAttrGet *cb, uint64_t session_id,
                 uint64_t handle, const char *name) {
  SessionDataPtr session_data;
  String node;
  int error=0;
  bool aborted=false, commited=false;
  String error_msg;
  DynamicBuffer dbuf;

  if (!get_session(session_id, session_data))
    HT_THROWF(Error::HYPERSPACE_EXPIRED_SESSION, "%llu", (Llu)session_id);

  if (m_verbose)
    HT_INFOF("attrget(session=%llu(%s), handle=%llu, name=%s)",
             (Llu)session_id, session_data->get_name(), (Llu)handle, name);

  HT_BDBTXN_BEGIN("attr_get-begin") {
    // (re) initialize vars
    aborted = false; commited = false;

    if (!m_bdb_fs->handle_exists(txn, handle)) {
      error = Error::HYPERSPACE_INVALID_HANDLE;
      error_msg = (String) "handle=" + handle;
      aborted = true;
      goto txn_commit;
    }

    m_bdb_fs->get_handle_node(txn, handle, node);
    if (!m_bdb_fs->get_xattr(txn, node, name, dbuf)) {
      error = Error::HYPERSPACE_ATTR_NOT_FOUND;
      error_msg = name;
      aborted = true;
      goto txn_commit;
    }

    txn_commit:
      if (aborted)
        txn->abort();
      else {
        txn->commit(0);
        commited = true;
      }
  }
  HT_BDBTXN_END_CB("attr_get-end", cb);

  if (aborted) {
    HT_DEBUG_OUT << "attrget(session=" << session_id << ", handle=" << handle << ", name='"
                 << name << "' ERROR = " << Error::get_text(error) << " " << error_msg
                 << HT_END;
    cb->error(error,error_msg);
    return;
  }

  StaticBuffer buffer(dbuf);

  HT_DEBUG_OUT << "attrget(session=" << session_id << ", handle=" << handle << ", name='"
               << name << "', value="<< (char *)buffer.base << HT_END;

  if ((error = cb->response(buffer)) != Error::OK)
    HT_ERRORF("Problem sending back response - %s", Error::get_text(error));
}

/**
 * attr_del does the following:
 *
 * > Make sure session is valid
 * > Make sure handle is valid
 * > Start BDB txn
 *   > Lock node data
 *   > Delete attribute in BDB
 *   > Deliver ATTR_DEL event notifications
 * > End BDB txn
 *
 */
void
Master::attr_del(ResponseCallback *cb, uint64_t session_id, uint64_t req_id, uint64_t handle,
                 const char *name) {
  SessionDataPtr session_data;
  String node;
  int error=0;
  String error_msg;
  bool aborted=false, commited=false;
  uint64_t event_id;
  HyperspaceEventPtr attr_del_event;
  NotificationMap attr_del_notifications;
  bool persisted_notifications = false;


  if (!get_session(session_id, session_data))
    HT_THROWF(Error::HYPERSPACE_EXPIRED_SESSION, "%llu", (Llu)session_id);

  if (m_verbose)
    HT_INFOF("attrdel(session=%llu(%s), handle=%llu, name=%s)",
             (Llu)session_id, session_data->get_name(), (Llu)handle, name);

  HT_BDBTXN_BEGIN("attr_del-begin") {
    // (re) initialize vars
    persisted_notifications = false; aborted = false; commited = false;
    ReqInfo req_info;

    if (!m_bdb_fs->handle_exists(txn, handle)) {
      error = Error::HYPERSPACE_INVALID_HANDLE;
      aborted = true;
      error_msg = (String)"handle=" + handle;
      goto txn_commit;
    }

    m_bdb_fs->get_handle_node(txn, handle, node);
    m_bdb_fs->del_xattr(txn, node, name);

    // create event notification and persist
    event_id = m_bdb_fs->get_next_id_i64(txn, EVENT_ID, true);
    m_bdb_fs->create_event(txn, EVENT_TYPE_NAMED, event_id, EVENT_MASK_ATTR_DEL, name);
    attr_del_event = new EventNamed(event_id, EVENT_MASK_ATTR_DEL, name);
    if (m_bdb_fs->get_node_event_notification_map(txn, node, EVENT_MASK_ATTR_DEL,
                                                  attr_del_notifications)) {
      persist_event_notifications(txn, event_id, attr_del_notifications);
      persisted_notifications = true;
      req_info.event_notifications.push_back(event_id);
    }
    // store req info
    req_info.type = Protocol::COMMAND_ATTRDEL;
    req_info.state = 1;
    m_bdb_fs->add_session_req(txn, session_id, req_id, req_info);

    txn_commit:
      if (aborted)
        txn->abort();
      else {
        txn->commit(0);
        commited = true;
      }
  }
  HT_BDBTXN_END_CB("attr_del-end", cb);

  // check for errors
  if (aborted) {
    cb->error(error, error_msg);
    return;
  }

  // deliver notifications
  if (commited && persisted_notifications) {
    deliver_event_notifications(attr_del_event, attr_del_notifications);
  }

  if ((error = cb->response_ok()) != Error::OK)
    HT_ERRORF("Problem sending back response - %s", Error::get_text(error));
}

void
Master::attr_exists(ResponseCallbackAttrExists *cb, uint64_t session_id, uint64_t handle,
                    const char *name)
{
  SessionDataPtr session_data;
  String node;
  int error = Error::OK;
  String error_msg;
  bool exists=false;
  bool aborted = false;

  if (m_verbose)
    HT_INFOF("attr_exists(session=%llu, handle=%llu, name=%s)",
             (Llu)session_id, (Llu)handle, name);

  if (!get_session(session_id, session_data))
    HT_THROWF(Error::HYPERSPACE_EXPIRED_SESSION, "%llu", (Llu)session_id);

  HT_BDBTXN_BEGIN("attr_exists-begin") {
    // (re) initialize vars
    aborted = false; exists = false;

    if (!m_bdb_fs->handle_exists(txn, handle)) {
      error = Error::HYPERSPACE_INVALID_HANDLE;
      error_msg = (String) "handle=" + handle;
      aborted = true;
      goto txn_commit;
    }

    m_bdb_fs->get_handle_node(txn, handle, node);

    if (m_bdb_fs->exists_xattr(txn, node, name))
      exists = true;

    txn_commit:
      if (aborted)
        txn->abort();
      else
        txn->commit(0);
  }
  HT_BDBTXN_END_CB("attr_exists-end", cb);

  if (aborted) {
    HT_ERROR_OUT << Error::get_text(error) << " - " << error_msg << HT_END;
    cb->error(error, error_msg);
    return;
  }

  if ((error = cb->response(exists)) != Error::OK)
    HT_ERRORF("Problem sending back response - %s", Error::get_text(error));
}

void
Master::attr_list(ResponseCallbackAttrList *cb, uint64_t session_id, uint64_t handle)
{
  SessionDataPtr session_data;
  String node;
  int error = 0;
  String error_msg;
  std::vector<String> attributes;
  bool aborted = false;

  if (m_verbose)
    HT_INFOF("attr_list(session=%llu, handle=%llu)", (Llu)session_id, (Llu)handle);

  if (!get_session(session_id, session_data))
    HT_THROWF(Error::HYPERSPACE_EXPIRED_SESSION, "%llu", (Llu)session_id);


  HT_BDBTXN_BEGIN("attr_list-begin") {
    aborted = false;

    if (!m_bdb_fs->handle_exists(txn, handle)) {
      error = Error::HYPERSPACE_INVALID_HANDLE;
      error_msg = (String) "handle=" + handle;
      aborted = true;
      goto txn_commit;
    }

    m_bdb_fs->get_handle_node(txn, handle, node);

    if (!m_bdb_fs->list_xattr(txn, node, attributes)) {
      error = Error::HYPERSPACE_ATTR_NOT_FOUND;
      error_msg = (String) "handle=" + handle + " node=" + node;
      aborted = true;
      goto txn_commit;
    }

    txn_commit:
      if (aborted)
        txn->abort();
      else
        txn->commit(0);
  }
  HT_BDBTXN_END_CB("attr_list-end", cb);

  if (aborted) {
    HT_ERROR_OUT << Error::get_text(error) << " - " << error_msg << HT_END;
    cb->error(error, error_msg);
    return;
  }

  if ((error = cb->response(attributes)) != Error::OK)
    HT_ERRORF("Problem sending back response - %s", Error::get_text(error));

}

/**
 * exists does the following:
 *
 * > Start BDB txn
 *   > Check if file exists in BDB
 * > End BDB txn
 *
 */
void
Master::exists(ResponseCallbackExists *cb, uint64_t session_id,
               const char *name) {
  int error;
  bool file_exists;

  if (m_verbose)
    HT_INFOF("exists(session_id=%llu, name=%s)", (Llu)session_id, name);

  assert(name[0] == '/' && name[strlen(name)-1] != '/');

  HT_BDBTXN_BEGIN("exists-begin") {
    file_exists = m_bdb_fs->exists(txn, name);
    txn->commit(0);
  }
  HT_BDBTXN_END_CB("exists-end", cb);

  if ((error = cb->response(file_exists)) != Error::OK)
    HT_ERRORF("Problem sending back response - %s", Error::get_text(error));

  if (m_verbose)
    HT_INFOF("exitting exists(session_id=%llu, name=%s)", (Llu)session_id, name);

}

/**
 * read_dir does the following:
 *
 * > Make sure session is valid
 * > Start BDB txn
 *   > Make sure handle is valid
 *   > Read dir data from BDB
 * > End BDB txn
 *
 */
void
Master::readdir(ResponseCallbackReaddir *cb, uint64_t session_id,
                uint64_t handle) {
  std::string abs_name;
  SessionDataPtr session_data;
  String node;
  int error = 0;
  String error_msg;
  bool aborted=false, commited=false;
  DirEntry dentry;
  std::vector<DirEntry> listing;

  if (!get_session(session_id, session_data))
    HT_THROWF(Error::HYPERSPACE_EXPIRED_SESSION, "%llu", (Llu)session_id);

  if (m_verbose)
    HT_INFOF("readdir(session=%llu(%s), handle=%llu)",
             (Llu)session_id, session_data->get_name(),(Llu)handle);

  HT_BDBTXN_BEGIN("readdir-begin") {
    if (!m_bdb_fs->handle_exists(txn, handle)) {
      error = Error::HYPERSPACE_INVALID_HANDLE;
      error_msg = (String) "handle=" + handle;
      aborted = true;
      goto txn_commit;
    }

    m_bdb_fs->get_handle_node(txn, handle, node);
    m_bdb_fs->get_directory_listing(txn, node, listing);

    txn_commit:
      if (aborted)
        txn->abort();
      else {
        txn->commit(0);
        commited = true;
      }
  }
  HT_BDBTXN_END_CB("readdir-end", cb);

  // check for errors
  if (aborted) {
    HT_ERROR_OUT << Error::get_text(error) << " - " << error_msg << HT_END;
    cb->error(error, error_msg);
    return;
  }

  cb->response(listing);
}

/**
 * lock
 */
void
Master::lock(ResponseCallbackLock *cb, uint64_t session_id, uint64_t req_id, uint64_t handle,
             uint32_t mode, bool try_lock) {
  SessionDataPtr session_data;
  bool notify = true;
  uint32_t open_flags, cur_lock_mode;
  String node;
  uint64_t lock_generation = 0;
  uint64_t event_id;
  HyperspaceEventPtr lock_acquired_event;
  NotificationMap lock_acquired_notifications;
  bool persisted_notifications = false;
  bool aborted=false, commited=false;
  int lock_status = 0;
  int error = Error::OK;
  String error_msg;

  if (!get_session(session_id, session_data)) {
    cb->error(Error::HYPERSPACE_EXPIRED_SESSION, "");
    return;
  }

  if (m_verbose) {
    HT_INFOF("lock(session=%llu(%s), handle=%llu, mode=0x%x, try_lock=%d)",
             (Llu)session_id, session_data->get_name(), (Llu)handle, mode, try_lock);
  }

  HT_BDBTXN_BEGIN("lock-begin") {
    // (re) initialize vars
    aborted = false; commited = false; persisted_notifications = false;
    lock_status=0; lock_generation = 0;
    uint8_t *ptr, *ret_val;
    ReqInfo req_info;
    uint32_t ret_val_size = sizeof(uint32_t) + sizeof(uint64_t);

    if (!m_bdb_fs->handle_exists(txn, handle)) {
      aborted = true;
      goto txn_commit;
    }

    open_flags = m_bdb_fs->get_handle_open_flags(txn, handle);
    if (!(open_flags & OPEN_FLAG_LOCK)) {
      error = Error::HYPERSPACE_MODE_RESTRICTION;
      error_msg = "handle not open for locking";
      aborted = true;
      goto txn_commit;
    }

    if (!(open_flags & OPEN_FLAG_WRITE)) {
      error = Error::HYPERSPACE_MODE_RESTRICTION;
      error_msg = "handle not open for writing";
      aborted = true;
      goto txn_commit;
    }

    m_bdb_fs->get_handle_node(txn, handle, node);
    cur_lock_mode = m_bdb_fs->get_node_cur_lock_mode(txn, node);
    if (cur_lock_mode == LOCK_MODE_EXCLUSIVE) {
      if (try_lock)
        lock_status = LOCK_STATUS_BUSY;
      else {
        // don't abort transaction since we need to persist pending lock req
        m_bdb_fs->add_node_pending_lock_request(txn, node, handle, mode);
        lock_status = LOCK_STATUS_PENDING;
      }
      goto txn_commit;
    }
    else if (cur_lock_mode == LOCK_MODE_SHARED) {
      if (mode == LOCK_MODE_EXCLUSIVE) {
        if (try_lock)
          lock_status = LOCK_STATUS_BUSY;
        else {
          // don't abort transaction since we need to persist pending lock req
          m_bdb_fs->add_node_pending_lock_request(txn, node, handle, mode);
          lock_status = LOCK_STATUS_PENDING;
        }
        goto txn_commit;
      }

      assert(mode == LOCK_MODE_SHARED);

      if (m_bdb_fs->node_has_pending_lock_request(txn, node)) {
        if (try_lock)
          lock_status = LOCK_STATUS_BUSY;
        else {
          // don't abort transaction since we need to persist pending lock req
          m_bdb_fs->add_node_pending_lock_request(txn, node, handle, mode);
          lock_status = LOCK_STATUS_PENDING;
        }
        goto txn_commit;
      }
    }

    // at this point we're OK to acquire the lock
    if (mode == LOCK_MODE_SHARED && m_bdb_fs->node_has_shared_lock_handles(txn, node))
      notify = false;

    lock_status = LOCK_STATUS_GRANTED;
    lock_generation = m_bdb_fs->incr_node_lock_generation(txn, node);

    m_bdb_fs->set_xattr_i64(txn, node, "lock.generation", lock_generation);
    m_bdb_fs->set_node_cur_lock_mode(txn, node, mode);
    lock_handle(txn, handle, mode, node);

    // create lock acquired event & persist event notifications
    if (notify) {
      event_id = m_bdb_fs->get_next_id_i64(txn, EVENT_ID, true);
      m_bdb_fs->create_event(txn, EVENT_TYPE_LOCK_ACQUIRED, event_id, EVENT_MASK_LOCK_ACQUIRED,
                             mode);
      lock_acquired_event = new EventLockAcquired(event_id, mode);
      if (m_bdb_fs->get_node_event_notification_map(txn, node, EVENT_MASK_LOCK_ACQUIRED,
                                                    lock_acquired_notifications)) {
        persist_event_notifications(txn, event_id, lock_acquired_notifications);
        persisted_notifications = true;
        req_info.event_notifications.push_back(event_id);
      }
    }

    // commit the txn
    txn_commit:
      if (aborted) {
        txn->abort();
        HT_DEBUG_OUT << "lock txn=" << txn << " aborted " << " handle=" << handle << " node="
                     << node << " mode=" << mode << " status=" << lock_status
                     << " lock_generation=" << lock_generation << HT_END;
      }
      else {
        // store req info
        req_info.type = Protocol::COMMAND_LOCK;
        req_info.state = 1;
        ptr = ret_val = new uint8_t[ret_val_size];
        Serialization::encode_i32(&ptr, lock_status);
        Serialization::encode_i64(&ptr, lock_generation);
        req_info.ret_val.set(ret_val, ret_val_size, true);
        m_bdb_fs->add_session_req(txn, session_id, req_id, req_info);

        txn->commit(0);
        commited = true;
        HT_DEBUG_OUT << "lock txn=" << txn << " commited " << " handle=" << handle << " node="
                     << node << " mode=" << mode << " status=" << lock_status
                     << " lock_generation=" << lock_generation << HT_END;
      }
  }
  HT_BDBTXN_END_CB("lock-end", cb);

  // check for errors
  if (aborted) {
    cb->error(error, error_msg);
    return;
  }

  // send lock request response
  switch (lock_status) {
    case LOCK_STATUS_GRANTED:
      cb->response(lock_status, lock_generation);
      break;
    default:
      cb->response(lock_status);
  }

  // deliver lock acquired event notifications
  if (commited && persisted_notifications) {
    deliver_event_notifications(lock_acquired_event, lock_acquired_notifications);
  }

}

/**
 * Assumes node is locked and BDB txn has started.
 * lock_handle does the following:
 * > If node name string is null then fill in the node name
 * > Set exclusive handle to acquiring handle or add acquiring handle to set of shared handles
 * > Set handle data to locked
 */
void Master::lock_handle(DbTxn *txn, uint64_t handle, uint32_t mode, String& node) {

  if (node == "")
    m_bdb_fs->get_handle_node(txn, handle, node);

  if (mode == LOCK_MODE_SHARED)
    m_bdb_fs->add_node_shared_lock_handle(txn, node, handle);
  else {
    assert(mode == LOCK_MODE_EXCLUSIVE);
    m_bdb_fs->set_node_exclusive_lock_handle(txn, node, handle);
  }
  m_bdb_fs->set_handle_locked(txn, handle, true);
}

/**
 * Assumes node is locked.
 * lock_handle does the following:
 * > Set exclusive handle to acquiring handle or add acquiring handle to set of shared handles
 * > Set handle data to locked
 */
void Master::lock_handle(DbTxn *txn, uint64_t handle, uint32_t mode, const String& node) {

  assert(node != "");
  if (mode == LOCK_MODE_SHARED)
    m_bdb_fs->add_node_shared_lock_handle(txn, node, handle);
  else {
    assert(mode == LOCK_MODE_EXCLUSIVE);
    m_bdb_fs->set_node_exclusive_lock_handle(txn, node, handle);
  }
  m_bdb_fs->set_handle_locked(txn, handle, true);
}


/**
 * release
 */
void
Master::release(ResponseCallback *cb, uint64_t session_id, uint64_t req_id, uint64_t handle) {
  SessionDataPtr session_data;
  String node;
  int error = 0;
  String error_msg;
  NotificationMap lock_release_notifications, lock_granted_notifications,
                  lock_acquired_notifications;
  HyperspaceEventPtr lock_release_event, lock_granted_event, lock_acquired_event;
  bool aborted = false, commited = false;
  ReqInfo req_info;
  uint8_t *args, *ptr;
  uint32_t args_size = sizeof(uint64_t);

  if (!get_session(session_id, session_data)) {
    cb->error(Error::HYPERSPACE_EXPIRED_SESSION, "");
    return;
  }

  if (m_verbose) {
    HT_INFOF("release(session=%llu(%s), handle=%llu)",
             (Llu)session_id, session_data->get_name(), (Llu)handle);
  }

  // txn 1: release lock
  HT_BDBTXN_BEGIN("release-begin-1") {
    if (!m_bdb_fs->handle_exists(txn, handle)) {
      error = Error::HYPERSPACE_INVALID_HANDLE;
      error_msg = (String) "handle=" + handle;
      aborted = true;
      goto txn_commit_1;
    }

    m_bdb_fs->get_handle_node(txn, handle, node);

    release_lock(txn, handle, node, lock_release_event, lock_release_notifications);
    // store req info
    req_info.type = Protocol::COMMAND_RELEASE;
    req_info.state = 1;
    ptr = args = new uint8_t[args_size];
    Serialization::encode_i64(&ptr, handle);
    req_info.args.set(args, args_size, true);
    m_bdb_fs->add_session_req(txn, session_id, req_id, req_info);
    if (lock_release_notifications.size() > 0)
        m_bdb_fs->add_session_req_event(txn, session_id, req_id, lock_release_event->get_id());

    txn_commit_1:
      if (aborted)
        txn->abort();
      else {
        txn->commit(0);
        commited = true;
      }
  }
  HT_BDBTXN_END_CB("release-end-1", cb);

  // check for errors
  if (aborted) {
    HT_ERROR_OUT << Error::get_text(error) << " - " << error_msg << HT_END;
    cb->error(error, error_msg);
    return;
  }

  // deliver lock released notifications
  if (commited)
    deliver_event_notifications(lock_release_event, lock_release_notifications);

  // txn 2: grant pending lock(s)
  HT_BDBTXN_BEGIN("release-begin-2") {
    grant_pending_lock_reqs(txn, node, lock_granted_event, lock_granted_notifications,
        lock_acquired_event, lock_acquired_notifications);

    // update req info
    req_info.state++;
    m_bdb_fs->update_session_req_state(txn, session_id, req_id, req_info.state);
    // delete lock release event notification if reqd
    if (lock_release_notifications.size() > 0)
      m_bdb_fs->delete_session_req_event(txn, session_id, req_id,
                                         lock_release_event->get_id());
    // add lock granted and acquired events
    if (lock_granted_notifications.size() > 0)
      m_bdb_fs->add_session_req_event(txn, session_id, req_id, lock_granted_event->get_id());
    if (lock_acquired_notifications.size() > 0)
      m_bdb_fs->add_session_req_event(txn, session_id, req_id,
                                      lock_acquired_event->get_id());
    // commit txn
    txn->commit(0);
  }
  HT_BDBTXN_END_CB("release-end-2", cb);

  // deliver lock granted & acquired notifications
  deliver_event_notifications(lock_granted_event, lock_granted_notifications);
  deliver_event_notifications(lock_acquired_event, lock_acquired_notifications);

  cb->response_ok();
}

/**
 * release_lock: does the following
 * > if handle is not locked return
 * > if lock is exclusive
 *   > set node exclusive lock handle to 0
 * > else
 *   > delete this shared lock handle from node
 * > set handle to unlocked
 * > if node has no shared lock handles (at this point it has no exclusive lock handle)
 *   > create a new LOCK_RELEASED event and persist in BDB
 *   > get map of handles -> sessions to be notified of LOCK_RELEASE event on this node
 *   > persist LOCK_RELEASE notifications
 *   > set node to unlocked in BDB
 *
 */
void Master::release_lock(DbTxn *txn, uint64_t handle, const String &node,
    HyperspaceEventPtr &release_event, NotificationMap &release_notifications) {
  vector<uint64_t> next_lock_handles;
  uint64_t exclusive_lock_handle=0;

  if (m_bdb_fs->handle_is_locked(txn, handle)) {
    exclusive_lock_handle = m_bdb_fs->get_node_exclusive_lock_handle(txn,node);
    if (exclusive_lock_handle != 0) {
      assert(handle == exclusive_lock_handle);
      m_bdb_fs->set_node_exclusive_lock_handle(txn, node, 0);
    }
    else {
      m_bdb_fs->delete_node_shared_lock_handle(txn, node, handle);
    }
    m_bdb_fs->set_handle_locked(txn, handle, false);
  }
  else
    return;

  // persist LOCK_RELEASED notifications if no more locks held on node
  if (!m_bdb_fs->node_has_shared_lock_handles(txn, node)) {
    HT_INFO("Persisting lock released notifications");
    uint64_t event_id = m_bdb_fs->get_next_id_i64(txn, EVENT_ID, true);
    release_event = new EventLockReleased(event_id);
    m_bdb_fs->create_event(txn, EVENT_TYPE_LOCK_RELEASED, event_id,
                           release_event->get_mask());
    if (m_bdb_fs->get_node_event_notification_map(txn, node, release_event->get_mask(),
                                                  release_notifications)) {
      persist_event_notifications(txn, event_id, release_notifications);
    }

    m_bdb_fs->set_node_cur_lock_mode(txn, node, 0);
    HT_INFO("Finished persisting lock released notifications");
  }
}

/**
 * grant_pending_lock_reqs does the following
 *
 * > Check if the node has pending locks
 * > Get the first pending exclusive lock or set of next pending shared locks
 * > Grant lock to next pending lock(s)
 * > Persist lock granted notifications
 * > Persist lock acquired notifications
 */
void Master::grant_pending_lock_reqs(DbTxn *txn, const String &node,
    HyperspaceEventPtr &lock_granted_event, NotificationMap &lock_granted_notifications,
    HyperspaceEventPtr &lock_acquired_event, NotificationMap &lock_acquired_notifications) {
  vector<uint64_t> next_lock_handles;
  int next_mode = 0;
  LockRequest front_lock_req;

  if (m_bdb_fs->get_node_pending_lock_request(txn, node, front_lock_req)) {
    next_mode = front_lock_req.mode;

    if (next_mode == LOCK_MODE_EXCLUSIVE) {
      // get the pending exclusive lock request
      next_lock_handles.push_back(front_lock_req.handle);
      m_bdb_fs->delete_node_pending_lock_request(txn, node, front_lock_req.handle);
    }
    else {
      // gather up all the pending shared lock requests preceeding the next exclusive request
      assert(next_mode == LOCK_MODE_SHARED);
      LockRequest lockreq = front_lock_req;
      do {
        if (lockreq.mode != LOCK_MODE_SHARED)
          break;
        next_lock_handles.push_back(lockreq.handle);
        m_bdb_fs->delete_node_pending_lock_request(txn, node, lockreq.handle);
      } while (m_bdb_fs->get_node_pending_lock_request(txn, node, lockreq));
    }

    if (!next_lock_handles.empty()) {
      // we have at least 1 pending lock request
      // grant lock to next pending locks and persist lock granted notifications
      uint64_t lock_generation = m_bdb_fs->incr_node_lock_generation(txn, node);
      uint64_t event_id = m_bdb_fs->get_next_id_i64(txn, EVENT_ID, true);
      uint64_t session;
      m_bdb_fs->create_event(txn, EVENT_TYPE_LOCK_GRANTED, event_id, EVENT_MASK_LOCK_GRANTED,
                             next_mode, lock_generation);
      m_bdb_fs->set_xattr_i64(txn, node, "lock.generation", lock_generation);
      m_bdb_fs->set_node_cur_lock_mode(txn, node, next_mode);

      lock_granted_event = new EventLockGranted(event_id, next_mode, lock_generation);

      foreach(uint64_t handle, next_lock_handles) {
        lock_handle(txn, handle, next_mode, node);
        session = m_bdb_fs->get_handle_session(txn, handle);
        lock_granted_notifications[handle] = session;
      }
      // persist lock granted notifications
      persist_event_notifications(txn, event_id, lock_granted_notifications);

      // create lock acquired event
      event_id = m_bdb_fs->get_next_id_i64(txn, EVENT_ID, true);
      m_bdb_fs->create_event(txn, EVENT_TYPE_LOCK_ACQUIRED, event_id, EVENT_MASK_LOCK_ACQUIRED,
                             next_mode);
      lock_acquired_event = new EventLockAcquired(event_id, next_mode);
      // persist lock acquired notifications
      if (m_bdb_fs->get_node_event_notification_map(txn, node, EVENT_MASK_LOCK_ACQUIRED,
                                                    lock_acquired_notifications))
        persist_event_notifications(txn, event_id, lock_acquired_notifications);
    }
  }
}

/**
 * Assumes it is in the middle of a BDB txn
 *
 * > Store the handles affected by an event in BerkeleyDB
 */
void
Master::persist_event_notifications(DbTxn *txn, uint64_t event_id,
                                    NotificationMap &handles_to_sessions)
{
  if (handles_to_sessions.size() > 0) {
    vector<uint64_t> handles;
    for (NotificationMap::iterator iter = handles_to_sessions.begin();
         iter != handles_to_sessions.end(); iter++) {
      handles.push_back(iter->first);
    }
    m_bdb_fs->set_event_notification_handles(txn, event_id, handles);
  }
}

/**
 * Assumes it is in the middle of a BDB txn
 *
 * > Store the handle addected by an event in BerkeleyDB
 */
void
Master::persist_event_notifications(DbTxn *txn, uint64_t event_id, uint64_t handle)
{
  vector<uint64_t> handles;
  handles.push_back(handle);
  m_bdb_fs->set_event_notification_handles(txn, event_id, handles);
}

/**
 *
 */
void
Master::deliver_event_notifications(HyperspaceEventPtr &event_ptr,
    NotificationMap &handles_to_sessions, bool wait_for_notify)
{
  SessionDataPtr session_data;
  uint64_t session_id;
  uint64_t handle_id;
  bool has_notifications = false;
  vector<uint64_t> sessions;

  for (NotificationMap::iterator iter = handles_to_sessions.begin();
       iter != handles_to_sessions.end(); iter++) {
    handle_id = iter->first;
    session_id = iter->second;
    if(get_session(session_id,session_data)) {
      session_data->add_notification(new Notification(handle_id, event_ptr ) );
      sessions.push_back(session_id);
      has_notifications = true;
    }
  }

  if (has_notifications) {

    foreach (session_id, sessions) {
      m_keepalive_handler_ptr->deliver_event_notifications(session_id);
    }

    if (wait_for_notify)
      event_ptr->wait_for_notifications();

    if (m_verbose)
      HT_INFO_OUT << "exitting deliver_event_notifications for "
                  << " event_id= " << event_ptr->get_id() << HT_END;
  }
  else {
    HT_INFO_OUT << "exitting deliver_event_notifications nothing to do"<< HT_END;
  }
}

/**
 *
 */
bool
Master::find_parent_node(const String &normal_name,String &parent_name, String &child_name) {
  size_t last_slash = normal_name.rfind("/", normal_name.length());

  child_name.clear();

  if (last_slash > 0) {
    parent_name = normal_name.substr(0, last_slash);
    child_name.append(normal_name, last_slash + 1, normal_name.length() - last_slash - 1);
    return true;
  }
  else if (last_slash == 0) {
    parent_name = "/";
    child_name.append(normal_name, 1, normal_name.length() - 1);
    return true;
  }

  return false;
}

/**
 * destroy_handle does the following:
 * > Start BDB txn
 * > Read in Node data
 * > Call release lock passing it the parent txn (update handle and node to unlocked,
 *       remove handle from node data, grant lock to next acquiring handles,
 *       persist LOCK_RELEASED, GRANTED and ACQUIRED notifications)
 * > Keep track of node id and delete handle data from BDB
 * > End BDB txn
 * > Deliver notifications
 *
 * > If no one else has this node open
 *   > if this is an ephemeral node
 *     > Start BDB txn
 *       > persist CHILD_NODE_REMOVED event notification
 *       > delete node from BDB
 *     > End BDB txn
 *
 * > Delete handle from BDB
 */
bool
Master::destroy_handle(uint64_t handle, int *errorp, std::string &errmsg, bool wait_for_notify,
                       uint64_t session_id, uint64_t req_id, uint32_t req_state) {
  bool has_refs = false;
  NotificationMap lock_release_notifications, lock_granted_notifications,
                  lock_acquired_notifications, node_removed_notifications;
  HyperspaceEventPtr lock_release_event, lock_granted_event, lock_acquired_event,
                     node_removed_event ;
  bool node_removed = false;
  String node;

  HT_DEBUG_OUT << "destroy_handle (handle=" << handle << ")" << HT_END;
  // either this is part of a request or not
  HT_ASSERT((session_id != 0 && req_id !=0 ) || (session_id ==0 && req_id ==0));

  // txn 1: release lock
  HT_BDBTXN_BEGIN("destroy_handle-begin-1") {
    m_bdb_fs->get_handle_node(txn, handle, node);
    m_bdb_fs->delete_node_handle(txn, node, handle);
    release_lock(txn, handle, node, lock_release_event, lock_release_notifications);
    m_bdb_fs->set_handle_del_state(txn, handle, HANDLE_UNLOCKED);

    // update request state if reqd
    if (req_id != 0) {
      ++req_state;
      m_bdb_fs->update_session_req_state(txn, session_id, req_id, req_state);
      // add event notification if reqd
      if (lock_release_notifications.size() > 0)
        m_bdb_fs->add_session_req_event(txn, session_id, req_id, lock_release_event->get_id());
    }
    txn->commit(0);
  }
  HT_BDBTXN_END("destroy_handle-end-1", false);

  // deliver lock released notifications
  deliver_event_notifications(lock_release_event, lock_release_notifications,
                              wait_for_notify);

  // txn 2: grant pending lock(s)
  HT_BDBTXN_BEGIN("destroy_handle-begin-2") {
    grant_pending_lock_reqs(txn, node, lock_granted_event, lock_granted_notifications,
        lock_acquired_event, lock_acquired_notifications);
    m_bdb_fs->set_handle_del_state(txn, handle, HANDLE_PENDING_LOCKS_GRANTED);

    // update request state if reqd
    if (req_id != 0) {
      ++req_state;
      m_bdb_fs->update_session_req_state(txn, session_id, req_id, req_state);
      // delete lock release event notification if reqd
      if (lock_release_notifications.size() > 0)
        m_bdb_fs->delete_session_req_event(txn, session_id, req_id,
                                           lock_release_event->get_id());
      // add lock granted and acquired events
      if (lock_granted_notifications.size() > 0)
        m_bdb_fs->add_session_req_event(txn, session_id, req_id, lock_granted_event->get_id());
      if (lock_acquired_notifications.size() > 0)
        m_bdb_fs->add_session_req_event(txn, session_id, req_id,
                                        lock_acquired_event->get_id());
    }
    txn->commit(0);
  }
  HT_BDBTXN_END("destroy_handle-end-2", false);

  // deliver lock granted & acquired notifications
  deliver_event_notifications(lock_granted_event, lock_granted_notifications, wait_for_notify);
  deliver_event_notifications(lock_acquired_event, lock_acquired_notifications,
                              wait_for_notify);

  // txn 3: delete node if ephemeral and no one has it open
  HT_BDBTXN_BEGIN("destroy_handle-begin-3") {
    has_refs = m_bdb_fs->node_has_open_handles(txn, node);
    if (!has_refs && m_bdb_fs->node_is_ephemeral(txn, node)) {
      String parent_node, child_node;

      if (find_parent_node(node, parent_node, child_node)) {
        // persist child node removed notifications
        uint64_t event_id = m_bdb_fs->get_next_id_i64(txn, EVENT_ID, true);
        m_bdb_fs->create_event(txn, EVENT_TYPE_NAMED, event_id,
                               EVENT_MASK_CHILD_NODE_REMOVED, child_node);
        node_removed_event = new EventNamed(event_id, EVENT_MASK_CHILD_NODE_REMOVED,
                                            child_node);
        if (m_bdb_fs->get_node_event_notification_map(txn, parent_node,
            EVENT_MASK_CHILD_NODE_REMOVED, node_removed_notifications)) {
          persist_event_notifications(txn, event_id, node_removed_notifications);
        }
        // unlink file and delete node data from BDB
        m_bdb_fs->unlink(txn, node);
        m_bdb_fs->delete_node(txn, node);
        node_removed = true;
      }
    }
    m_bdb_fs->set_handle_del_state(txn, handle, HANDLE_EPHEMERAL_NODE_DELETED);

    // update request state if reqd
    if (req_id != 0) {
      ++req_state;
      m_bdb_fs->update_session_req_state(txn, session_id, req_id, req_state);
      // delete lock granted and acquired events
      if (lock_granted_notifications.size() > 0)
        m_bdb_fs->delete_session_req_event(txn, session_id, req_id,
                                           lock_granted_event->get_id());
      if (lock_acquired_notifications.size() > 0)
        m_bdb_fs->delete_session_req_event(txn, session_id, req_id,
                                           lock_acquired_event->get_id());
      // add node removed notification if reqd
      if (node_removed_notifications.size() > 0)
        m_bdb_fs->add_session_req_event(txn, session_id, req_id, node_removed_event->get_id());
    }
    txn->commit(0);
  }
  HT_BDBTXN_END("destroy_handle-end-3", false);
  // deliver node removed notifications
  if (node_removed) {
    deliver_event_notifications(node_removed_event, node_removed_notifications,
                                wait_for_notify);
  }

  // txn 4: delete handle data from BDB
  HT_BDBTXN_BEGIN("destroy_handle-begin-4") {
    m_bdb_fs->delete_handle(txn, handle);
    // update request state if reqd
    if (req_id != 0) {
      ++req_state;
      m_bdb_fs->update_session_req_state(txn, session_id, req_id, req_state);
      // delete node removed notification if reqd
      if (node_removed_notifications.size() > 0)
        m_bdb_fs->delete_session_req_event(txn, session_id, req_id,
                                           node_removed_event->get_id());
    }
    txn->commit(0);
  }
  HT_BDBTXN_END("destroy_handle-end-4", false);

  return true;
}


/**
 */
void Master::get_generation_number() {

  HT_BDBTXN_BEGIN("get_generation_number-begin") {
    if (!m_bdb_fs->get_xattr_i32(txn, "/hyperspace/metadata", "generation",
                                 &m_generation))
      m_generation = 0;

    m_generation++;
    m_bdb_fs->set_xattr_i32(txn, "/hyperspace/metadata", "generation",
                            m_generation);
    txn->commit(0);
  }
  HT_BDBTXN_END("get_generation_number-end", );
}

/**
 * If node exists but corresponding node data doesn't then
 * create the node data
 */
bool
Master::validate_and_create_node_data(DbTxn *txn, const String &node)
{
  // make sure node is exists
  if (!m_bdb_fs->exists(txn, node)) {
    return false;
  }

  // create node data for this node and set its lock generation
  if (!m_bdb_fs->node_exists(txn, node)) {
    uint64_t lock_generation;
    if (!m_bdb_fs->get_xattr_i64(txn, node, "lock.generation", &lock_generation)) {
      lock_generation = 1;
      m_bdb_fs->set_xattr_i64(txn, node, "lock.generation", lock_generation);
    }
    m_bdb_fs->create_node(txn, node, false, lock_generation);
  }

  return true;
}


