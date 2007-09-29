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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "Common/Error.h"
#include "Common/Logger.h"

#include "AsyncComm/ResponseCallback.h"
#include "AsyncComm/Serialization.h"

#include "Master.h"
#include "RequestHandlerRelease.h"

using namespace Hyperspace;
using namespace hypertable;

/**
 *
 */
void RequestHandlerRelease::run() {
  ResponseCallback cb(mComm, mEventPtr);
  size_t remaining = mEventPtr->messageLen - 2;
  uint8_t *msgPtr = mEventPtr->message + 2;
  uint64_t handle;

  // handle
  if (!Serialization::DecodeLong(&msgPtr, &remaining, &handle))
    goto abort;

  mMaster->Release(&cb, mSessionId, handle);

  return;

 abort:
  LOG_ERROR("Encoding problem with RELEASE message");
  cb.error(Error::PROTOCOL_ERROR, "Encoding problem with RELEASE message");
}