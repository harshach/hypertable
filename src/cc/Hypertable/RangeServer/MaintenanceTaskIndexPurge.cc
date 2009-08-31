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
#include "MaintenanceTaskIndexPurge.h"

using namespace Hypertable;

/**
 *
 */
MaintenanceTaskIndexPurge::MaintenanceTaskIndexPurge(boost::xtime &stime,
                                                     RangePtr &range,
                                                     int64_t scanner_generation)
  : MaintenanceTask(stime, range, String("INDEX PURGE ") + range->get_name()),
    m_scanner_generation(scanner_generation) {
}


/**
 *
 */
void MaintenanceTaskIndexPurge::execute() {
  m_range->purge_index_data(m_scanner_generation);
}