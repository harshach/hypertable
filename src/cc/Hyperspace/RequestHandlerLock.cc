/**
 * Copyright (C) 2007 Doug Judd (Zvents, Inc.)
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
#include "Common/Error.h"
#include "Common/Logger.h"

#include "AsyncComm/ResponseCallback.h"
#include "Common/Serialization.h"

#include "Master.h"
#include "RequestHandlerLock.h"
#include "ResponseCallbackLock.h"

using namespace Hyperspace;
using namespace Hypertable;
using namespace Serialization;

/**
 *
 */
void RequestHandlerLock::run() {
  ResponseCallbackLock cb(m_comm, m_event_ptr);
  size_t decode_remain = m_event_ptr->payload_len;
  const uint8_t *decode_ptr = m_event_ptr->payload;

  try {
    uint64_t req_id = decode_i64(&decode_ptr, &decode_remain);
    uint64_t handle = decode_i64(&decode_ptr, &decode_remain);
    uint32_t mode = decode_i32(&decode_ptr, &decode_remain);
    bool try_lock = decode_i8(&decode_ptr, &decode_remain);

    m_master->lock(&cb, m_session_id, req_id, handle, mode, try_lock);
  }
  catch (Exception &e) {
    HT_ERROR_OUT << e << HT_END;
    cb.error(e.code(), "Error handling LOCK message");
  }
}
