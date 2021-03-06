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
#include "Common/Serialization.h"

#include <boost/shared_array.hpp>

#include "CellStoreFactory.h"
#include "CellStoreV0.h"
#include "CellStoreV1.h"
#include "CellStoreTrailerV0.h"
#include "CellStoreTrailerV1.h"
#include "Global.h"

using namespace Hypertable;

CellStore *CellStoreFactory::open(const String &name,
                                  const char *start_row, const char *end_row) {
  String start = (start_row) ? start_row : "";
  String end = (end_row) ? end_row : Key::END_ROW_MARKER;
  int64_t file_length;
  int32_t fd;
  size_t nread, amount;
  uint64_t offset;
  uint16_t version;

  /** Get the file length **/
  file_length = Global::dfs->length(name);

  /** Open the DFS file **/
  fd = Global::dfs->open(name);

  amount = (file_length < 512) ? file_length : 512;
  offset = file_length - amount;

  boost::shared_array<uint8_t> trailer_buf( new uint8_t [amount] );

  nread = Global::dfs->pread(fd, trailer_buf.get(), amount, offset);

  if (nread != amount)
    HT_THROWF(Error::DFSBROKER_IO_ERROR,
              "Problem reading trailer for CellStore file '%s'"
              " - only read %d of %lu bytes", name.c_str(),
              (int)nread, (Lu)amount);

  const uint8_t *ptr = trailer_buf.get() + (amount-2);
  size_t remaining = 2;

  version = Serialization::decode_i16(&ptr, &remaining);

  if (version == 1) {
    CellStoreTrailerV1 trailer_v1;
    CellStoreV1 *cellstore_v1;

    if (amount < trailer_v1.size())
      HT_THROWF(Error::RANGESERVER_CORRUPT_CELLSTORE,
                "Bad length of CellStoreV1 file '%s' - %llu",
                name.c_str(), (Llu)file_length);

    trailer_v1.deserialize(trailer_buf.get() + (amount - trailer_v1.size()));

    cellstore_v1 = new CellStoreV1(Global::dfs);
    cellstore_v1->open(name, start, end, fd, file_length, &trailer_v1);
    return cellstore_v1;
  }
  else if (version == 0) {
    CellStoreTrailerV0 trailer_v0;
    CellStoreV0 *cellstore_v0;

    if (amount < trailer_v0.size())
      HT_THROWF(Error::RANGESERVER_CORRUPT_CELLSTORE,
                "Bad length of CellStoreV0 file '%s' - %llu",
                name.c_str(), (Llu)file_length);

    trailer_v0.deserialize(trailer_buf.get() + (amount - trailer_v0.size()));

    cellstore_v0 = new CellStoreV0(Global::dfs);
    cellstore_v0->open(name, start, end, fd, file_length, &trailer_v0);
    return cellstore_v0;
  }
  return 0;
}
