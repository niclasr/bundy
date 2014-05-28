# Copyright (C) 2013  Internet Systems Consortium.
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SYSTEMS CONSORTIUM
# DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL
# INTERNET SYSTEMS CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT,
# INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING
# FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
# NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
# WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

import unittest
import errno
import os
import signal

import bundy.log
import bundy.config
from bundy.server_common.bundy_server import *
from bundy.testutils.ccsession_mock import MockModuleCCSession

TEST_FILENO = 42                # arbitrarily chosen

class TestException(Exception):
    """A generic exception class specific in this test module."""
    pass

class MyCCSession(MockModuleCCSession, bundy.config.ConfigData):
    def __init__(self, specfile, config_handler, command_handler):
        # record parameter for later inspection
        self.specfile_param = specfile
        self.config_handler_param = config_handler
        self.command_handler_param = command_handler

        self.check_command_param = None # used in check_command()

        # Initialize some local attributes of MockModuleCCSession, including
        # 'stopped'
        MockModuleCCSession.__init__(self)

    def start(self):
        pass

    def check_command(self, nonblock):
        """Mock check_command(). Just record the param for later inspection."""
        self.check_command_param = nonblock

    def get_socket(self):
        return self

    def fileno(self):
        """Pretending get_socket().fileno()

        Returing an arbitrarily chosen constant.

        """
        return TEST_FILENO

class MockServer(BUNDYServer):
    def __init__(self):
        BUNDYServer.__init__(self)
        self._select_fn = self.select_wrapper

    def _setup_ccsession(self):
        orig_cls = bundy.config.ModuleCCSession
        bundy.config.ModuleCCSession = MyCCSession
        try:
            super()._setup_ccsession()
        except Exception:
            raise
        finally:
            bundy.config.ModuleCCSession = orig_cls

    def _config_handler(self):
        pass

    def mod_command_handler(self, cmd, args):
        """A sample _mod_command_handler implementation."""
        self.command_handler_params = (cmd, args) # for inspection
        return bundy.config.create_answer(0)

    def select_wrapper(self, reads, writes, errors):
        self._trigger_shutdown() # make sure the loop will stop
        self.select_params = (reads, writes, errors) # record for inspection
        return [], [], []

class TestBUNDYServer(unittest.TestCase):
    def setUp(self):
        self.__server = MockServer()
        self.__reads = 0
        self.__writes = 0
        self.__errors = 0

    def test_init(self):
        """Check initial conditions"""
        self.assertFalse(self.__server.shutdown)

    def test_trigger_shutdown(self):
        self.__server._trigger_shutdown()
        self.assertTrue(self.__server.shutdown)

    def test_sigterm_handler(self):
        """Check the signal handler behavior.

        SIGTERM and SIGINT should be caught and should call memmgr's
        _trigger_shutdown().  This test also indirectly confirms run() calls
        run_internal().

        """
        def checker():
            self.__shutdown_called = True

        self.__server._run_internal = lambda: os.kill(os.getpid(),
                                                      signal.SIGTERM)
        self.__server._trigger_shutdown = lambda: checker()
        self.assertEqual(0, self.__server.run('test'))
        self.assertTrue(self.__shutdown_called)

        self.__shutdown_called = False
        self.__server._run_internal = lambda: os.kill(os.getpid(),
                                                      signal.SIGINT)
        self.assertEqual(0, self.__server.run('test'))
        self.assertTrue(self.__shutdown_called)

    def test_exception(self):
        """Check exceptions are handled, not leaked."""
        def exception_raiser(ex_cls):
            raise ex_cls('test')

        # Test all possible exceptions that are explicitly caught
        for ex in [TestException, BUNDYServerFatal]:
            self.__server._run_internal = lambda: exception_raiser(ex)
            self.assertEqual(1, self.__server.run('test'))

    def test_run(self):
        """Check other behavior of run()"""
        self.__server._run_internal = lambda: None # prevent looping
        self.assertEqual(0, self.__server.run('test'))
        # module CC session should have been setup.
        # The exact path to the spec file can vary, so we simply check
        # it works and it's the expected name stripping the path.
        self.assertEqual(
            self.__server.mod_ccsession.specfile_param.split('/')[-1],
            'test.spec')
        self.assertEqual(self.__server.mod_ccsession.config_handler_param,
                         self.__server._config_handler)
        self.assertEqual(self.__server.mod_ccsession.command_handler_param,
                         self.__server._command_handler)

    def test_run_with_setup_module(self):
        """Check run() with module specific setup method."""
        self.setup_called = False
        def check_called():
            self.setup_called = True
        self.__server._run_internal = lambda: None
        self.__server._setup_module = check_called
        self.assertEqual(0, self.__server.run('test'))
        self.assertTrue(self.setup_called)

    def test_shutdown_command(self):
        answer = self.__server._command_handler('shutdown', None)
        self.assertTrue(self.__server.shutdown)
        self.assertEqual((0, None), bundy.config.parse_answer(answer))

    def test_run_with_shutdown_module(self):
        """Check run() with module specific shutdown method."""
        self.shutdown_called = False
        def check_called():
            self.shutdown_called = True
        self.__server.__shutdown = True
        self.__server._shutdown_module = check_called
        self.assertEqual(0, self.__server.run('test'))
        self.assertTrue(self.shutdown_called)

    def test_other_command(self):
        self.__server._mod_command_handler = self.__server.mod_command_handler
        answer = self.__server._command_handler('other command', None)
        # shouldn't be confused with shutdown
        self.assertFalse(self.__server.shutdown)
        self.assertEqual((0, None), bundy.config.parse_answer(answer))
        self.assertEqual(('other command', None),
                         self.__server.command_handler_params)

    def test_other_command_nohandler(self):
        """Similar to test_other_command, but without explicit handler"""
        # In this case "unknown command" error should be returned.
        answer = self.__server._command_handler('other command', None)
        self.assertEqual(1, bundy.config.parse_answer(answer)[0])

    def test_run_internal(self):
        self.__server._setup_ccsession()
        self.__server._run_internal()
        self.assertEqual(([TEST_FILENO], [], []), self.__server.select_params)

    def select_wrapper(self, r, w, e, ex=None, ret=None):
        """Mock select() function used in some of the tests below.

        If ex is not None and it's first call to this method, it raises ex
        assuming it's an exception.

        If ret is not None, it returns the given value; otherwise it returns
        all empty lists.

        """
        self.select_params.append((r, w, e))
        if ex is not None and len(self.select_params) == 1:
            raise ex
        else:
            self.__server._trigger_shutdown()
        if ret is not None:
            return ret
        return [], [], []

    def test_select_for_command(self):
        """A normal event iteration, handling one command."""
        self.select_params = []
        self.__server._select_fn = \
            lambda r, w, e: self.select_wrapper(r, w, e,
                                                ret=([TEST_FILENO], [], []))
        self.__server._setup_ccsession()
        self.__server._run_internal()
        # select should be called only once.
        self.assertEqual([([TEST_FILENO], [], [])], self.select_params)
        # check_command should have been called.
        self.assertTrue(self.__server.mod_ccsession.check_command_param)
        # module CC session should have been stopped explicitly.
        self.assertTrue(self.__server.mod_ccsession.stopped)

    def test_select_interrupted(self):
        """Emulating case select() raises EINTR."""
        self.select_params = []
        self.__server._select_fn = \
            lambda r, w, e: self.select_wrapper(r, w, e,
                                                ex=select.error(errno.EINTR))
        self.__server._setup_ccsession()
        self.__server._run_internal()
        # EINTR will be ignored and select() will be called again.
        self.assertEqual([([TEST_FILENO], [], []), ([TEST_FILENO], [], [])],
                          self.select_params)
        # check_command() shouldn't have been called (select_wrapper returns
        # empty lists by default).
        self.assertIsNone(self.__server.mod_ccsession.check_command_param)
        self.assertTrue(self.__server.mod_ccsession.stopped)

    def test_select_other_exception(self):
        """Emulating case select() raises other select error."""
        self.select_params = []
        self.__server._select_fn = \
            lambda r, w, e: self.select_wrapper(r, w, e,
                                                ex=select.error(errno.EBADF))
        self.__server._setup_ccsession()
        # the exception will be propagated.
        self.assertRaises(select.error, self.__server._run_internal)
        self.assertEqual([([TEST_FILENO], [], [])], self.select_params)
        # in this case module CC session hasn't been stopped explicitly
        # others will notice it due to connection reset.
        self.assertFalse(self.__server.mod_ccsession.stopped)

    def my_read_callback(self):
        self.__reads += 1

    def my_write_callback(self):
        self.__writes += 1

    def my_error_callback(self):
        self.__errors += 1

    def test_watch_fileno(self):
        """Test watching for fileno."""
        self.select_params = []
        self.__server._select_fn = \
            lambda r, w, e: self.select_wrapper(r, w, e,
                                                ret=([10, 20, 42, TEST_FILENO],
                                                     [], [30]))
        self.__server._setup_ccsession()

        self.__server.watch_fileno(10, rcallback=self.my_read_callback)
        self.__server.watch_fileno(20, rcallback=self.my_read_callback, \
                                       wcallback=self.my_write_callback)
        self.__server.watch_fileno(30, xcallback=self.my_error_callback)

        self.__server._run_internal()
        self.assertEqual([([10, 20, TEST_FILENO], [20], [30])],
                         self.select_params)
        self.assertEqual(2, self.__reads)
        self.assertEqual(0, self.__writes)
        self.assertEqual(1, self.__errors)

    # Unified helper for test_unwatch_fileno.  mode is either 'r', 'w', or 'x',
    # meaning checking for read/write/error callbacks, respectively.
    def __check_unwatch(self, mode):
        self.__server = MockServer() # re-initialize so we can reuse it
        self.__unwatch_called = 0
        orig_trigger_shutdown = self.__server._trigger_shutdown

        # We call unwatch_fileno() in a callback.
        def unwatch_wrapper(fileno):
            self.__unwatch_called += 1
            self.__server.unwatch_fileno(fileno, mode == 'r', mode == 'w',
                                         mode == 'x')
        # This will be called from the faked select_wrapper, making sure
        # we'll have two iterations of the select loop.
        def trigger_shutdown():
            self.__server._trigger_shutdown = orig_trigger_shutdown
        self.__server._trigger_shutdown = trigger_shutdown

        # Usual setup with a faked select function
        self.select_params = []
        self.__server._select_fn = \
            lambda r, w, e: self.select_wrapper(r, w, e, ret=([10, 20],
                                                              [10, 20],
                                                              [10, 20]))
        self.__server._setup_ccsession()

        # Specify callbacks, depending on the mode.  One is to call
        # unwatch_fileno().  The other is to confirm calling unwatch_fileno
        # doesn't cause disruption for other callbacks (if any) for that fileno.
        rcb = lambda: unwatch_wrapper(10) if mode == 'r' else None
        wcb = lambda: unwatch_wrapper(10) if mode == 'w' else None
        xcb = lambda: unwatch_wrapper(10) if mode == 'x' else None
        self.__server.watch_fileno(10, rcallback=rcb, wcallback=wcb,
                                   xcallback=xcb)
        self.__server.watch_fileno(10, rcallback=self.my_read_callback,
                                   wcallback=self.my_write_callback,
                                   xcallback=self.my_error_callback)

        # Run the loop, and check unwatch is really called exactly once.
        # Since there should be two iterations, this means unwatch works
        # as expected.
        self.__server._run_internal()
        self.assertEqual(1, self.__unwatch_called)

    def test_unwatch_fileno(self):
        self.__check_unwatch('r')
        self.__check_unwatch('w')
        self.__check_unwatch('x')

    def test_bad_unwatch_fileno(self):
        """Check if unwatch_fileno raises for bad fileno."""
        self.__server.watch_fileno(10, rcallback=self.my_read_callback)
        self.assertRaises(ValueError, self.__server.unwatch_fileno, 20, True,
                          False, False)
        self.__server.watch_fileno(10, wcallback=self.my_write_callback)
        self.assertRaises(ValueError, self.__server.unwatch_fileno, 20, False,
                          True, False)
        self.__server.watch_fileno(10, xcallback=self.my_error_callback)
        self.assertRaises(ValueError, self.__server.unwatch_fileno, 20, False,
                          False, True)

if __name__== "__main__":
    bundy.log.init("bundy_server_test")
    bundy.log.resetUnitTestRootLogger()
    unittest.main()
