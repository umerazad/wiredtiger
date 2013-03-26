#!/usr/bin/env python
#
# Public Domain 2008-2013 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
# If unittest2 is available, use it in preference to (the old) unittest
try:
    import unittest2 as unittest
except ImportError:
    import unittest

import os
import shutil
import wiredtiger, wttest
from helper import config_check
from test_shared_cache import openConnections, closeConnections, addRecords

# test_shared_cache_reconf.py
# Test reconfiguring cache shared amongst multiple connections.
class test_shared_cache_reconf(wttest.WiredTigerTestCase):

    uri = 'table:test_shared_cache_reconf'
    # Setup fairly large items to use up cache
    data_str = 'abcdefghijklmnopqrstuvwxyz' * 20

    # Disable default setup/shutdown steps - connections are managed manually.
    def setUpSessionOpen(self, conn):
        return None

    def close_conn(self):
        return None

    def setUpConnectionOpen(self, dir):
        return None

    def common_test(self, reconfig, reconfig2 = None):
        """
        Create some connections using a shared cache, do some work and then
        reconfigure. reconfig are any options for the reconfigure.
        """

        nops = 1000
        openConnections(self, ['WT_TEST1', 'WT_TEST2'])

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")

        # Add enough data so that the cache is used.
        for i in range(20):
            for sess in self.sessions:
                addRecords(self, sess, i * nops, (i + 1) * nops)

        connection = self.conns[0]
        connection.reconfigure(reconfig)

        # TODO: Verify the configuration is correct.
        # self.session = self.sessions[0]
        # self.assertTrue(config_check(self, None, "size", "300M"))

        # If reconfig2 is set then update the configuration again
        if reconfig2 != None:
            for i in range(5):
                for sess in self.sessions:
                    addRecords(self, sess, i * nops, (i + 1) * nops)
            connection.reconfigure(reconfig)
            # TODO: Verify the configuration is correct.

        closeConnections(self, )

    # Test reconfigure of shared cache
    def test_shared_cache_reconf01(self):
        self.common_test("shared_cache=(size=300M)")

    def test_shared_cache_reconf02(self):
        self.common_test("shared_cache=(chunk=20M)")

    def test_shared_cache_reconf03(self):
        self.common_test("shared_cache=(reserve=50M)")

    def test_shared_cache_reconf04(self):
        self.common_test("shared_cache=(reserve=50M)", "shared_cache=(reserve=15M)")

if __name__ == '__main__':
    wttest.run()

