#!/usr/bin/env bash
#
# Copyright 2010 Sanjit Jhala (Hypertable, Inc.)
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# The installation directory
export HYPERTABLE_HOME=$(cd `dirname "$0"`/.. && pwd)

usage() {
  echo ""
  echo "usage: start-monitoring.sh [OPTIONS] [<server-options>]"
  echo ""
  echo "OPTIONS:"
  echo ""
}

cd $HYPERTABLE_HOME/Monitoring/
command="rackup config.ru -p 3000 -P $HYPERTABLE_HOME/run/MonitoringServer.pid 2>&1 >/dev/null "
echo "Current dir `pwd`"
echo "Command: $command"
nohup $command &
