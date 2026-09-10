"""Regression checks for measurement and scenario tools; no desktop input."""

import contextlib
import enum
import hashlib
import importlib.util
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import types
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
# These tests exercise orchestration with an explicit fake platform. They run
# on Linux and Windows too and never read or move the user's real pointer.
sys.modules['scripts.scenarios.quartz'] = types.ModuleType('quartz')
sys.modules['scripts.scenarios.quartz'].ProcessIdentityUnavailable = type(
    'ProcessIdentityUnavailable', (RuntimeError,), {})
from scripts.scenarios import catalog, conditions, content, run, session


def load_script(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


compare = load_script('bench_compare', 'scripts/bench-compare.py')


class MacSignal(enum.IntEnum):
    SIGTERM = 15
    SIGKILL = 9


class ScenarioPreferencesTests(unittest.TestCase):
    def test_stack_is_serialized_as_module_ids(self):
        text = session.preferences_text('WVR', (10, 20, 440, 720))
        fields = dict(line.split('=', 1) for line in text.splitlines())
        self.assertEqual(fields['scope_stack'],
                         '[org.sidescopes.waveform][org.sidescopes.vectorscope][org.sidescopes.parade]')
        self.assertEqual(fields['window_x'], '10')
        self.assertEqual(fields['window_y'], '20')

    def test_duplicate_letters_do_not_duplicate_scopes(self):
        text = session.preferences_text('VVW', (0, 0, 440, 720))
        self.assertIn('scope_stack=[org.sidescopes.vectorscope][org.sidescopes.waveform]\n', text)

    def test_empty_region_is_established_explicitly(self):
        with mock.patch.object(session.quartz, 'press_key', create=True) as press:
            self.assertTrue(session.establish_region(42, 'none', (), (), {}))
        press.assert_called_once_with('escape')

class ApplicationTeardownTests(unittest.TestCase):
    def setUp(self):
        self.scope = contextlib.ExitStack()
        self.addCleanup(self.scope.close)
        self.identity = ('/owned/SideScopes.app/Contents/MacOS/SideScopes', 1234)
        self.alive = True
        self.clock = 0.0
        self.lookup = self.scope.enter_context(mock.patch.object(
            session.quartz, 'process_identity', side_effect=lambda pid: self.identity if self.alive else None,
            create=True))
        self.request = self.scope.enter_context(mock.patch.object(
            session.quartz, 'request_quit', return_value=True, create=True))
        self.signal = self.scope.enter_context(mock.patch.object(session.os, 'kill'))
        # The simulated macOS process uses macOS signals even when its tests
        # run on Windows, whose native signal module has no SIGKILL.
        self.scope.enter_context(mock.patch.object(session, 'signal', MacSignal))
        self.scope.enter_context(mock.patch.object(session.time, 'monotonic', side_effect=lambda: self.clock))
        self.scope.enter_context(mock.patch.object(session.time, 'sleep', side_effect=self.advance))
        self.scope.enter_context(mock.patch.object(session, '_GRACEFUL_QUIT_TIMEOUT_SECONDS', 0.4))
        self.scope.enter_context(mock.patch.object(session, '_SIGNAL_QUIT_TIMEOUT_SECONDS', 0.4))

    def advance(self, seconds):
        self.clock += seconds

    def exit(self, *_):
        self.alive = False
        return True

    def test_normal_request_confirms_exit_without_signals(self):
        self.request.side_effect = self.exit
        outcome = session.quit_application(42, identity=self.identity)
        self.assertEqual(outcome['exit'], 'graceful')
        self.assertEqual(outcome['identity_retries'], 0)
        self.assertEqual(session.measurement_method(True)['teardown'],
                         'pid-quit-confirmed/2; legacy-signal-first')
        self.request.assert_called_once_with(42)
        self.signal.assert_not_called()

    def test_already_exited_needs_no_request_or_signal(self):
        self.alive = False
        self.assertEqual(session.quit_application(42)['exit'], 'already-exited')
        self.request.assert_not_called()
        self.signal.assert_not_called()

    def test_slow_bounded_shutdown_has_time_to_finish_without_signals(self):
        self.lookup.side_effect = lambda pid: self.identity if self.clock < 16 else None
        with mock.patch.object(session, '_GRACEFUL_QUIT_TIMEOUT_SECONDS', 20), \
                mock.patch.object(session, '_SIGNAL_QUIT_TIMEOUT_SECONDS', 8):
            outcome = session.quit_application(42, identity=self.identity)
            self.assertEqual(session.measurement_method(True)['teardown_timeouts_seconds'],
                             {'graceful': 20, 'signal': 8})
        self.assertEqual(outcome['exit'], 'graceful')
        self.assertGreaterEqual(self.clock, 16)
        self.assertLess(self.clock, 17)
        self.signal.assert_not_called()

    def test_each_signal_keeps_its_separate_bounded_wait(self):
        sent = []
        self.signal.side_effect = lambda pid, sig: sent.append((sig, self.clock))
        with mock.patch.object(session, '_GRACEFUL_QUIT_TIMEOUT_SECONDS', 20), \
                mock.patch.object(session, '_SIGNAL_QUIT_TIMEOUT_SECONDS', 8):
            with self.assertRaisesRegex(RuntimeError, 'did not exit'):
                session.quit_application(42, identity=self.identity)
        self.assertEqual([sig for sig, _ in sent], [session.signal.SIGTERM, session.signal.SIGKILL])
        self.assertAlmostEqual(sent[0][1], 20, delta=0.3)
        self.assertAlmostEqual(sent[1][1] - sent[0][1], 8, delta=0.3)
        self.assertAlmostEqual(self.clock - sent[1][1], 8, delta=0.3)

    def test_unknown_or_reused_target_is_never_signalled(self):
        for failure in (PermissionError('denied'), None):
            with self.subTest(failure=failure):
                self.lookup.side_effect = failure
                self.lookup.return_value = ('/other', 999)
                with self.assertRaises((PermissionError, RuntimeError)):
                    session.quit_application(42, identity=self.identity)
                self.request.assert_not_called()
                self.signal.assert_not_called()

    def test_unavailable_identity_then_confirmed_exit_without_signals(self):
        for initial in (False, True):
            with self.subTest(initial=initial):
                self.clock = 0.0
                self.request.reset_mock()
                self.lookup.side_effect = ([session.quartz.ProcessIdentityUnavailable('exiting'), None]
                                          if initial else [self.identity,
                                          session.quartz.ProcessIdentityUnavailable('exiting'), None])
                outcome = session.quit_application(42, identity=self.identity)
                self.assertEqual(outcome['exit'], 'already-exited' if initial else 'graceful')
                self.assertEqual(outcome['identity_retries'], 1)
                self.assertAlmostEqual(self.clock, 0.2)
                self.assertEqual(self.request.call_count, 0 if initial else 1)
                self.signal.assert_not_called()

    def test_unavailable_identity_then_same_live_target_exits_normally(self):
        self.lookup.side_effect = [self.identity, session.quartz.ProcessIdentityUnavailable('exiting'),
                                  self.identity, None]
        outcome = session.quit_application(42, identity=self.identity)
        self.assertEqual(outcome['exit'], 'graceful')
        self.assertEqual(outcome['identity_retries'], 1)
        self.assertAlmostEqual(self.clock, 0.4)
        self.signal.assert_not_called()

    def test_persistent_unavailable_identity_exhausts_shared_budget_without_signals(self):
        for stage in ('initial', 'graceful-wait', 'legacy-initial'):
            with self.subTest(stage=stage):
                self.clock = 0.0
                self.request.reset_mock()
                calls = 0

                def identity(pid):
                    nonlocal calls
                    calls += 1
                    if stage == 'graceful-wait' and calls == 1:
                        return self.identity
                    raise session.quartz.ProcessIdentityUnavailable('unavailable')

                self.lookup.side_effect = identity
                with self.assertRaises(session.quartz.ProcessIdentityUnavailable) as raised:
                    session.quit_application(42, identity=self.identity, graceful=stage != 'legacy-initial')
                self.assertEqual(raised.exception.teardown['exit'], 'unconfirmed')
                self.assertEqual(raised.exception.teardown['identity_retries'], 2)
                self.assertAlmostEqual(self.clock, 0.4)
                self.signal.assert_not_called()

    def test_initial_retry_does_not_extend_graceful_stage(self):
        calls = 0

        def identity(pid):
            nonlocal calls
            calls += 1
            if calls == 1:
                raise session.quartz.ProcessIdentityUnavailable('transient')
            return self.identity if self.alive else None

        self.lookup.side_effect = identity
        signals = []

        def terminate(pid, sig):
            signals.append(self.clock)
            self.exit()

        self.signal.side_effect = terminate
        outcome = session.quit_application(42, identity=self.identity)
        self.assertEqual(outcome['exit'], 'signal')
        self.assertEqual(signals, [0.4])

    def test_replacement_after_unavailable_identity_fails_immediately(self):
        self.lookup.side_effect = [self.identity, session.quartz.ProcessIdentityUnavailable('transient'),
                                  ('/replacement', 999)]
        with self.assertRaisesRegex(RuntimeError, 'identity changed'):
            session.quit_application(42, identity=self.identity)
        self.assertAlmostEqual(self.clock, 0.2)
        self.signal.assert_not_called()

    def test_unspecified_identity_is_adopted_only_after_successful_inspection(self):
        self.lookup.side_effect = [session.quartz.ProcessIdentityUnavailable('transient'), self.identity, None]
        outcome = session.quit_application(42)
        self.assertEqual(outcome['exit'], 'graceful')
        self.assertEqual(outcome['identity_retries'], 1)
        self.request.assert_called_once_with(42)
        self.signal.assert_not_called()

    def test_signal_stage_unavailable_identity_never_signals_an_unknown_target(self):
        calls = 0

        def identity(pid):
            nonlocal calls
            calls += 1
            # Initial lookup, then known live at 0, .2 and .4; ambiguity
            # starts precisely at the new TERM stage's safety check.
            if calls <= 4:
                return self.identity
            raise session.quartz.ProcessIdentityUnavailable('unavailable')

        self.lookup.side_effect = identity
        with self.assertRaises(session.quartz.ProcessIdentityUnavailable):
            session.quit_application(42, identity=self.identity)
        self.assertAlmostEqual(self.clock, 0.8)
        self.signal.assert_not_called()

    def test_identity_recovered_at_signal_deadline_does_not_send_a_signal(self):
        calls = 0

        def identity(pid):
            nonlocal calls
            calls += 1
            if calls in (5, 6):
                raise session.quartz.ProcessIdentityUnavailable('transient')
            return self.identity

        self.lookup.side_effect = identity
        with self.assertRaisesRegex(RuntimeError, 'exhausted the SIGTERM budget'):
            session.quit_application(42, identity=self.identity)
        self.assertAlmostEqual(self.clock, 0.8)
        self.signal.assert_not_called()

    def test_initial_identity_recovered_at_deadline_does_not_request_quit(self):
        self.lookup.side_effect = [session.quartz.ProcessIdentityUnavailable('transient'),
                                  session.quartz.ProcessIdentityUnavailable('transient'), self.identity]
        with self.assertRaisesRegex(RuntimeError, 'exhausted the initial quit budget'):
            session.quit_application(42, identity=self.identity)
        self.assertAlmostEqual(self.clock, 0.4)
        self.request.assert_not_called()
        self.signal.assert_not_called()

    def test_unavailable_identity_after_signals_stops_at_current_stage_deadline(self):
        for after in (session.signal.SIGTERM, session.signal.SIGKILL):
            with self.subTest(after=after):
                self.clock = 0.0
                self.signal.reset_mock()
                uncertain = False

                def identity(pid):
                    if uncertain:
                        raise session.quartz.ProcessIdentityUnavailable('unavailable')
                    return self.identity

                def signal(pid, sig):
                    nonlocal uncertain
                    uncertain = sig == after

                self.lookup.side_effect = identity
                self.signal.side_effect = signal
                with self.assertRaises(session.quartz.ProcessIdentityUnavailable) as raised:
                    session.quit_application(42, identity=self.identity)
                expected = ['SIGTERM'] if after == session.signal.SIGTERM else ['SIGTERM', 'SIGKILL']
                self.assertEqual(raised.exception.teardown['signals'], expected)
                self.assertEqual(raised.exception.teardown['exit'], 'unconfirmed')
                self.assertAlmostEqual(self.clock, 0.8 if after == session.signal.SIGTERM else 1.2)

    def test_programming_error_is_not_retried(self):
        self.lookup.side_effect = [self.identity, ValueError('broken inspection')]
        with self.assertRaisesRegex(ValueError, 'broken inspection'):
            session.quit_application(42, identity=self.identity)
        self.assertEqual(self.clock, 0.0)
        self.signal.assert_not_called()

    def test_rejected_or_failed_request_records_term_fallback(self):
        for failure in (False, RuntimeError('request unavailable')):
            with self.subTest(failure=failure):
                self.alive = True
                self.request.side_effect = failure if isinstance(failure, Exception) else None
                self.request.return_value = False
                self.signal.side_effect = self.exit
                outcome = session.quit_application(42, identity=self.identity)
                self.assertEqual(outcome['exit'], 'signal')
                self.assertEqual(outcome['signals'], ['SIGTERM'])
                self.assertFalse(outcome['request_sent'])
                self.assertEqual('request_error' in outcome, isinstance(failure, Exception))

    def test_timeouts_reach_kill_and_confirm_its_exit(self):
        def kill(pid, sig):
            if sig == session.signal.SIGKILL:
                self.exit()
        self.signal.side_effect = kill
        outcome = session.quit_application(42, identity=self.identity)
        self.assertEqual(outcome['signals'], ['SIGTERM', 'SIGKILL'])
        self.assertEqual(outcome['exit'], 'signal')
        self.assertFalse(self.alive)

    def test_surviving_kill_fails_after_a_bounded_wait(self):
        with self.assertRaisesRegex(RuntimeError, 'did not exit'):
            session.quit_application(42, identity=self.identity)
        self.assertEqual([call.args[1] for call in self.signal.call_args_list],
                         [session.signal.SIGTERM, session.signal.SIGKILL])
        self.assertLess(self.clock, 2)

    def test_identity_change_before_fallback_does_not_signal_replacement(self):
        def request(pid):
            self.identity = ('/replacement', 999)
            return True
        self.request.side_effect = request
        with self.assertRaisesRegex(RuntimeError, 'identity changed'):
            session.quit_application(42, identity=self.identity)
        self.signal.assert_not_called()

    def test_interrupted_request_or_wait_cleans_up_before_propagating(self):
        for stage in ('request', 'graceful-wait', 'term-wait'):
            with self.subTest(stage=stage):
                self.alive = True
                self.request.side_effect = KeyboardInterrupt() if stage == 'request' else None
                self.signal.reset_mock()
                if stage == 'term-wait':
                    self.signal.side_effect = lambda pid, sig: self.exit() if sig == session.signal.SIGKILL else None
                else:
                    self.signal.side_effect = self.exit
                interrupted = False

                def sleep(seconds):
                    nonlocal interrupted
                    ready = stage == 'graceful-wait' or (stage == 'term-wait' and self.signal.called)
                    if ready and not interrupted:
                        interrupted = True
                        raise KeyboardInterrupt()
                    self.advance(seconds)

                with mock.patch.object(session.time, 'sleep', side_effect=sleep):
                    with self.assertRaises(KeyboardInterrupt) as raised:
                        session.quit_application(42, identity=self.identity)
                self.assertFalse(self.alive)
                self.assertEqual(raised.exception.teardown['interrupted'], 'KeyboardInterrupt')
                self.assertEqual(raised.exception.teardown['exit'], 'signal')
                self.assertTrue(self.signal.called)

    def test_legacy_build_uses_signals_without_a_normal_request(self):
        self.signal.side_effect = self.exit
        outcome = session.quit_application(42, identity=self.identity, graceful=False)
        self.assertEqual(outcome['method'], 'legacy-signal')
        self.assertEqual(outcome['signals'], ['SIGTERM'])
        self.request.assert_not_called()


class ApplicationQuitBindingTests(unittest.TestCase):
    def setUp(self):
        with mock.patch('ctypes.CDLL', return_value=mock.MagicMock()):
            self.bindings = load_script('quit_bindings', 'scripts/scenarios/quartz.py')

    def test_pid_request_releases_pool_for_success_nil_and_exception(self):
        for target, failure in ((99, None), (None, None), (99, RuntimeError('request'))):
            with self.subTest(target=target, failure=failure):
                objects, lookup, request, release = (mock.Mock(return_value=42), mock.Mock(return_value=target),
                                                     mock.Mock(return_value=True, side_effect=failure), mock.Mock())
                api = (lambda name: name, lambda name: name, objects, lookup, request, release)
                with mock.patch.object(self.bindings, '_quit_bindings', return_value=api):
                    if failure:
                        with self.assertRaises(RuntimeError):
                            self.bindings.request_quit(123)
                    else:
                        self.assertEqual(self.bindings.request_quit(123), bool(target))
                lookup.assert_called_once_with(b'NSRunningApplication',
                                               b'runningApplicationWithProcessIdentifier:', 123)
                release.assert_called_once_with(42, b'drain')
                if target:
                    request.assert_called_once_with(99, b'terminate')
                else:
                    request.assert_not_called()

    def test_identity_reads_kernel_start_time_and_path_without_hiding_native_errors(self):
        def usage(pid, version, buffer):
            buffer._obj[80:88] = (987654321).to_bytes(8, 'little')
            return 0

        def path(pid, buffer, size):
            buffer.value = b'/owned/SideScopes.app/Contents/MacOS/SideScopes'
            return len(buffer.value)

        with mock.patch.object(self.bindings, 'process_exists', return_value=True) as exists, \
                mock.patch.object(self.bindings._libc, 'proc_pid_rusage', side_effect=usage) as sample, \
                mock.patch.object(self.bindings._libc, 'proc_pidpath', side_effect=path) as executable:
            self.assertEqual(self.bindings.process_identity(42),
                             ('/owned/SideScopes.app/Contents/MacOS/SideScopes', 987654321))
            self.assertEqual(sample.call_args.args[:2], (42, 0))
            self.assertEqual(executable.call_args.args[0], 42)
            for failing_call in (sample, executable):
                with self.subTest(failing_call=failing_call):
                    previous = failing_call.side_effect
                    failing_call.side_effect = None
                    failing_call.return_value = -1
                    with self.assertRaisesRegex(self.bindings.ProcessIdentityUnavailable, 'cannot establish identity'):
                        self.bindings.process_identity(42)
                    exists.side_effect = [True, False]
                    self.assertIsNone(self.bindings.process_identity(42))
                    exists.side_effect = None
                    failing_call.side_effect = previous

    def test_liveness_distinguishes_exit_from_inspection_error(self):
        with mock.patch.object(self.bindings.os, 'kill') as probe:
            self.assertTrue(self.bindings.process_exists(42))
            probe.side_effect = ProcessLookupError()
            self.assertFalse(self.bindings.process_exists(42))
            probe.side_effect = PermissionError()
            with self.assertRaises(PermissionError):
                self.bindings.process_exists(42)
        with mock.patch.object(self.bindings.os, 'kill') as probe:
            for pid in (0, -1, '42'):
                with self.assertRaises(ValueError):
                    self.bindings.process_exists(pid)
                with self.assertRaises(ValueError):
                    self.bindings.request_quit(pid)
            probe.assert_not_called()


class ScenarioCleanupTests(unittest.TestCase):
    def exercise(self, action_failure=None, quit_failure=None, outcome=None, setup_failure=None):
        bundle = pathlib.Path('/owned/SideScopes.app')
        scenario = types.SimpleNamespace(content='still', content_fps=0, from_launch=setup_failure is None,
                                         action='still', region='draw', expects_analysis=False)
        rect = (10, 20, 300, 400)
        plan = types.SimpleNamespace(application_rect=rect, content_rect=rect, region_in=lambda _: rect)
        guard, window, action = mock.Mock(), mock.Mock(pid=43, rect=rect), mock.Mock()
        action.stop.side_effect = action_failure
        action.complaints.return_value = []
        measurement = session.Measurement(1, 2, 3, 4, 5, 6, measurement_method=session.measurement_method(False))
        default = dict(method='appkit-pid', request_sent=True, signals=[], exit='graceful')
        with contextlib.ExitStack() as scope:
            for owner, name, values in (
                    (content, 'ContentWindow', dict(return_value=window)),
                    (run, '_motion_complaints', dict(return_value=[])),
                    (session, 'graceful_teardown', dict(return_value=True)),
                    (session, 'launch', dict(return_value=42)),
                    (session, 'await_window', dict(return_value=setup_failure != 'window')),
                    (session, 'establish_region', dict(return_value=setup_failure != 'region')),
                    (session.time, 'sleep', {}),
                    (session, 'action_for', dict(return_value=action)),
                    (session, 'measure', dict(return_value=measurement)),
                    (session, 'quit_application', dict(return_value=outcome or default, side_effect=quit_failure)),
                    (session.quartz, 'process_identity', dict(return_value=(str(session._executable(bundle)), 123))),
                    (session.quartz, 'move_pointer', {})):
                scope.enter_context(mock.patch.object(owner, name, create=True, **values))
            scope.enter_context(contextlib.redirect_stdout(io.StringIO()))
            try:
                result = run._run_one(scenario, 'V', bundle, plan, guard, None, None, None, 1)
            finally:
                window.stop.assert_called_once()
                self.assertEqual(guard.write.call_count, 1)
                if setup_failure:
                    action.start.assert_not_called()
                    session.measure.assert_not_called()
                    session.quit_application.assert_called_once()
                    if setup_failure == 'window':
                        session.establish_region.assert_not_called()
        return result

    def test_missing_window_or_region_stops_before_action_and_measurement(self):
        for stage, message in (('window', 'window never appeared'), ('region', 'requested draw region')):
            with self.subTest(stage=stage), self.assertRaisesRegex(RuntimeError, message):
                self.exercise(setup_failure=stage)

    def test_unresolved_actor_or_application_still_cleans_content_and_raises(self):
        for actor, app in ((RuntimeError('actor alive'), None), (None, RuntimeError('app alive'))):
            with self.subTest(actor=actor, app=app), self.assertRaises(RuntimeError):
                self.exercise(actor, app)

    def test_fallback_is_exported_and_marks_all_rows_incomparable(self):
        outcome = dict(method='appkit-pid', request_sent=True, signals=['SIGTERM'], exit='signal')
        result = self.exercise(outcome=outcome)
        result.scenario = catalog.scenario_named('idle-region')
        rows = run._rows(result, dict(machine='m', os='o', build='b', version='v'))
        self.assertTrue(result.warnings)
        self.assertTrue(all(not row['comparable'] and row['teardown'] == outcome for row in rows))
        self.assertEqual(rows[0]['measurement_method']['teardown_mode'], 'appkit-pid')

    def test_cleanup_failure_prevents_the_next_scenario(self):
        setup = dict(profile=None, scopes='V', seconds=1, bundle=None, plan=None, guard=None,
                     helper=None, content=None, diagnostics=None, build=None)
        with mock.patch.object(catalog, 'unavailable', return_value=''), \
                mock.patch.object(run, '_run_one', side_effect=RuntimeError('cleanup failed')) as launch, \
                contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(RuntimeError):
                run._measure_all([catalog.scenario_named('idle-region')] * 2, ['V'], setup)
            self.assertEqual(launch.call_count, 1)

    def test_public_cohort_stops_after_a_retained_fallback_result(self):
        result = self.exercise(outcome=dict(method='appkit-pid', request_sent=True,
                                           signals=['SIGTERM'], exit='signal'))
        result.scenario = catalog.scenario_named('idle-region')
        setup = dict(profile=None, scopes='V', seconds=1, bundle=None, plan=None, guard=None,
                     helper=None, content=None, diagnostics=None,
                     build=dict(machine='m', os='o', build='b', version='v'))
        with mock.patch.object(catalog, 'unavailable', return_value=''), \
                mock.patch.object(run, '_run_one', return_value=result) as launch, \
                contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, 'refusing the next scenario'):
                run._measure_all([result.scenario] * 2, ['V'], setup)
            self.assertEqual(launch.call_count, 1)

    def test_content_kill_wait_is_bounded_and_streams_close_on_failure(self):
        window = content.ContentWindow.__new__(content.ContentWindow)
        window._process = mock.Mock()
        window._process.poll.return_value = None
        window._process.wait.side_effect = subprocess.TimeoutExpired('helper', 5)
        with self.assertRaises(subprocess.TimeoutExpired):
            window.stop()
        window._process.kill.assert_called_once()
        self.assertEqual(window._process.wait.call_args_list, [mock.call(timeout=5), mock.call(timeout=5)])
        window._process.stdout.close.assert_called_once()
        window._process.stderr.close.assert_called_once()

    def test_actor_exception_is_rethrown_after_join(self):
        for failure in (RuntimeError('input failed'), KeyboardInterrupt()):
            with self.subTest(failure=failure):
                action = session.Action()
                action._loop = mock.Mock(side_effect=failure)
                action.start()
                with self.assertRaisesRegex(RuntimeError, 'thread failed') as raised:
                    action.stop()
                self.assertIs(raised.exception.__cause__, failure)
                self.assertFalse(action._thread.is_alive())

    def test_interrupted_content_start_still_cleans_its_owned_process(self):
        process = mock.Mock()
        process.poll.return_value = None
        with mock.patch.object(content.subprocess, 'Popen', return_value=process), \
                mock.patch.object(content.ContentWindow, '_read_header', side_effect=KeyboardInterrupt()):
            with self.assertRaises(KeyboardInterrupt):
                content.ContentWindow('unused', (0, 0, 10, 10), content.ContentSet('synthetic'))
        process.terminate.assert_called_once()
        process.wait.assert_called_once_with(timeout=5)
        process.stdout.close.assert_called_once()

    def test_actor_stop_detects_an_unjoined_thread(self):
        action = session.Action()
        action._thread = mock.Mock()
        action._thread.is_alive.return_value = True
        with self.assertRaisesRegex(RuntimeError, 'thread did not stop'):
            action.stop()
        self.assertTrue(action._stop.is_set())
        action._thread.join.assert_called_once_with(timeout=10.0)


class PreferenceRecoveryTests(unittest.TestCase):
    def test_override_saves_do_not_carry_over_and_original_is_restored(self):
        for originally_present in (True, False):
            with self.subTest(originally_present=originally_present), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                user = root / 'user.conf'
                original = b'owner preferences\n'
                if originally_present:
                    user.write_bytes(original)
                with mock.patch.object(run, 'PREFERENCES', user):
                    guard = run.PreferencesGuard(root)
                    guard.write(session.preferences_text('V', (1, 2, 3, 4)))
                    guard.override.write_text('app saved a changed window and stack\n')
                    next_text = session.preferences_text('W', (1, 2, 3, 4))
                    guard.write(next_text)
                    self.assertEqual(guard.override.read_text(), next_text)
                    guard.restore()
                self.assertEqual(user.read_bytes() if user.exists() else None,
                                 original if originally_present else None)

    def test_live_application_defers_restore_without_losing_backup(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            user = root / 'user.conf'
            user.write_text('original')
            with mock.patch.object(run, 'PREFERENCES', user), \
                    mock.patch.object(run.quartz, 'process_exists', return_value=True, create=True) as alive:
                guard = run.PreferencesGuard(root)
                guard.write(session.preferences_text('V', (1, 2, 3, 4)))
                guard.active_pid = 42
                with self.assertRaisesRegex(RuntimeError, 'backup retained'):
                    guard.restore()
                self.assertEqual(guard._backup.read_text(), 'original')
                with self.assertRaises(RuntimeError):
                    guard.write('next scenario')
                alive.return_value = False
                guard.restore()
                self.assertEqual(user.read_text(), 'original')

    def test_unidentified_launch_preserves_backup_and_refuses_reuse(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            user = root / 'user.conf'
            user.write_text('original')
            with mock.patch.object(run, 'PREFERENCES', user):
                guard = run.PreferencesGuard(root)
                guard.write(session.preferences_text('V', (1, 2, 3, 4)))
                before = user.read_bytes()
                guard.pending_launch = True
                with self.assertRaisesRegex(RuntimeError, 'before its PID was known'):
                    guard.restore()
                with self.assertRaises(RuntimeError):
                    guard.write('next scenario')
                self.assertEqual(user.read_bytes(), before)
                self.assertEqual(guard._backup.read_text(), 'original')

    def test_legacy_signal_disposition_preserves_interrupted_recovery_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            user = root / 'user.conf'
            user.write_text('original')
            bundle = root / 'SideScopes.app'
            executable = session._executable(bundle)
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b'old build without override')
            self.assertFalse(session.graceful_teardown(bundle))
            with mock.patch.object(run, 'PREFERENCES', user):
                guard = run.PreferencesGuard(root)
                guard.write(session.preferences_text('V', (1, 2, 3, 4)))
                # Signal-first shutdown does not save and strip the marker.
                recovered = run.PreferencesGuard(root)
                recovered.restore()
                self.assertEqual(user.read_text(), 'original')
            executable.write_bytes(b'SIDESCOPES_PREFS_FILE')
            self.assertTrue(session.graceful_teardown(bundle))
            self.assertNotEqual(session.measurement_method(True, graceful=True),
                                session.measurement_method(True, graceful=False))


class InputCleanupTests(unittest.TestCase):
    def test_interrupted_mouse_gestures_release_the_button(self):
        with mock.patch('ctypes.CDLL', return_value=mock.MagicMock()):
            bindings = load_script('quartz_mouse_bindings', 'scripts/scenarios/quartz.py')
        for gesture, pause_count in [('click', 2), ('drag', 5)]:
            for pause in range(pause_count):
                events = []
                with self.subTest(gesture=gesture, pause=pause), \
                        mock.patch.object(bindings, '_post_mouse', side_effect=lambda kind, point: events.append(kind)), \
                        mock.patch.object(bindings.time, 'sleep', side_effect=[None] * pause + [KeyboardInterrupt()]):
                    with self.assertRaises(KeyboardInterrupt):
                        if gesture == 'click':
                            bindings.click((10, 20))
                        else:
                            bindings.drag((10, 20), (40, 50), steps=2)
                    if bindings._LEFT_DOWN in events:
                        self.assertEqual(events[-1], bindings._LEFT_UP)

    def test_flick_sampling_failure_releases_the_button(self):
        action = session.BorderFlick(session.Target(pid=1), (10, 20, 100, 100), distance=200)
        with mock.patch.object(session, 'border_window', return_value=(0, 0, 135, 155)), \
                mock.patch.object(session, 'quartz') as native, \
                mock.patch.object(session.time, 'sleep'), \
                mock.patch.object(action, '_sample', side_effect=RuntimeError('sample failed')):
            with self.assertRaisesRegex(RuntimeError, 'sample failed'):
                action._flick(200)
            native.press_mouse.assert_called_once()
            native.release_mouse.assert_called_once()
            self.assertEqual(native.release_mouse.call_args, native.drag_mouse.call_args)

    def test_interrupted_chords_release_every_pressed_key(self):
        # Load the real binding with inert framework functions. No test input
        # reaches the desktop, on macOS or on the other CI platforms.
        with mock.patch('ctypes.CDLL', return_value=mock.MagicMock()):
            bindings = load_script('quartz_bindings', 'scripts/scenarios/quartz.py')
        for interrupted_pause in range(3):
            held = set()

            def post(code, pressed, flags):
                if pressed:
                    held.add(code)
                else:
                    held.discard(code)

            pauses = [None] * interrupted_pause + [KeyboardInterrupt()]
            with self.subTest(pause=interrupted_pause), \
                    mock.patch.object(bindings, '_post_key', side_effect=post), \
                    mock.patch.object(bindings.time, 'sleep', side_effect=pauses):
                with self.assertRaises(KeyboardInterrupt):
                    bindings.press_key('d', command=True, shift=True)
                self.assertEqual(held, set())


@unittest.skipIf(sys.platform == 'win32', 'the content helper uses macOS/POSIX pipes')
class ContentWindowTests(unittest.TestCase):
    def launch_fake(self, code):
        popen = subprocess.Popen
        process = popen([sys.executable, '-c', code], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True)
        return process

    def test_readiness_deadline_interrupts_an_incomplete_line_and_cleans_up(self):
        process = self.launch_fake("import sys,time; sys.stdout.write('partial'); sys.stdout.flush(); time.sleep(30)")
        with mock.patch.object(content.subprocess, 'Popen', return_value=process), \
                mock.patch.object(content, '_HEADER_TIMEOUT_SECONDS', 0.1):
            with self.assertRaisesRegex(RuntimeError, 'never reported itself ready'):
                content.ContentWindow('unused', (0, 0, 100, 100), content.ContentSet('synthetic'))
        self.assertIsNotNone(process.poll())
        self.assertTrue(process.stdout.closed)

    def test_readiness_parses_geometry_and_motion_before_returning(self):
        process = self.launch_fake("import time; print('content_rect 10,20,640,480\\npan 4,160,24\\nready',flush=True); time.sleep(30)")
        with mock.patch.object(content.subprocess, 'Popen', return_value=process):
            with content.ContentWindow('unused', (0, 0, 100, 100), content.ContentSet('synthetic')) as window:
                self.assertEqual(window.rect, (10, 20, 640, 480))
                self.assertEqual(window.pan['pixels_per_frame'], 4)
        self.assertIsNotNone(process.poll())


class CachedContentTests(unittest.TestCase):
    def test_helper_cache_uses_source_contents_instead_of_timestamps(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = pathlib.Path(directory)
            source = cache / 'source.m'
            source.write_text('first source')
            original_time = source.stat().st_mtime

            def compile_helper(command, **kwargs):
                pathlib.Path(command[command.index('-o') + 1]).write_text(source.read_text())
                return subprocess.CompletedProcess(command, 0, '', '')

            with mock.patch.object(content, 'HELPER_SOURCE', source), \
                    mock.patch.object(content.shutil, 'which', return_value='cc'), \
                    mock.patch.object(content.subprocess, 'run', side_effect=compile_helper) as compiler:
                binary = content.build_helper(cache)
                self.assertEqual(binary.read_text(), 'first source')
                content.build_helper(cache)
                self.assertEqual(compiler.call_count, 1)
                source.write_text('other source')
                os.utime(source, (original_time, original_time))
                binary = content.build_helper(cache)
                self.assertEqual(binary.read_text(), 'other source')
                self.assertEqual(compiler.call_count, 2)

    def test_failed_replacement_does_not_leave_corrupt_cached_content_available(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = pathlib.Path(directory)
            image = cache / 'sample.jpg'
            image.write_bytes(b'wrong image')
            entry = {'name': image.name, 'sha256': hashlib.sha256(b'right image').hexdigest(), 'url': 'unused'}
            with mock.patch.object(content, '_download', side_effect=OSError('offline')):
                with self.assertRaises(OSError):
                    content._fetch(entry, cache)
            self.assertFalse(image.exists())


class ComparisonTests(unittest.TestCase):
    def test_flat_rows_from_different_hosts_are_not_compared(self):
        before = {'value': 1, 'machine': 'laptop', 'os': 'macOS'}
        for field, value in [('machine', 'desktop'), ('os', 'Windows')]:
            with self.subTest(field=field):
                regression, output = self.compare(before, dict(before, value=2, **{field: value}))
                self.assertFalse(regression)
                self.assertIn('NOT COMPARABLE', output)
                self.assertIn(field, output)

    def test_content_identity_and_power_state_affect_comparability(self):
        before = {'conditions': {'content': {'kind': 'synthetic', 'patterns': ['gray']},
                                  'power': {'charging': False, 'cpu_speed_limit': 100}}}
        after = {'conditions': {'content': {'kind': 'synthetic', 'patterns': ['noise']},
                                 'power': {'charging': True, 'cpu_speed_limit': 80}}}
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(compare.report_conditions(before, after), ['power', 'content'])

    def compare(self, left, right, **kwargs):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            regression = compare.compare({'cost': left}, {'cost': right}, 15, **kwargs)
        return regression, output.getvalue()

    def test_a_new_cost_above_zero_is_a_regression(self):
        regression, _ = self.compare({'value': 0, 'unit': 'cores'}, {'value': 0.5, 'unit': 'cores'})
        self.assertTrue(regression)

    def test_different_units_are_not_compared(self):
        regression, output = self.compare({'value': 1, 'unit': 'seconds'}, {'value': 1000, 'unit': 'ms'})
        self.assertFalse(regression)
        self.assertIn('NOT COMPARABLE', output)

    def test_different_conditions_suppress_the_verdict(self):
        regression, output = self.compare({'value': 1}, {'value': 2}, condition_reason='different machines')
        self.assertFalse(regression)
        self.assertIn('NOT COMPARABLE: different machines', output)

    def test_worker_joined_envelope_is_not_compared_with_legacy_rows(self):
        legacy = {'value': 1, 'unit': 'cores'}
        joined = dict(legacy, value=2, measurement_method='worker-start-stop-join-v1')
        regression, output = self.compare(legacy, joined)
        self.assertFalse(regression)
        self.assertIn('NOT COMPARABLE: measurement methods differ', output)
        regression, output = self.compare(dict(joined, value=1), joined)
        self.assertTrue(regression)
        self.assertNotIn('NOT COMPARABLE', output)

    def test_last_pass_diagnostic_does_not_gate_a_regression(self):
        last = {'value': 1, 'unit': 'ms', 'measurement_method': 'worker-start-stop-join-v1',
                'statistic': 'last-pass', 'direction': 'none'}
        regression, output = self.compare(last, dict(last, value=100))
        self.assertFalse(regression)
        self.assertIn('(informational)', output)

    def test_method_quality_and_diagnostics_mismatches_are_not_compared(self):
        with mock.patch.dict(os.environ, {session.QUALITY_VARIABLE: 'standard'}):
            method = session.measurement_method(True)
        alternatives = [None, dict(method, version='other'),
                        dict(method, quality_override={session.QUALITY_VARIABLE: 'high'}),
                        dict(method, diagnostics={'enabled': False, 'flush': 'disabled'}),
                        dict(method, diagnostics={'enabled': True, 'flush': 'interval'})]
        for alternative in alternatives:
            left, right = {'value': 1, 'measurement_method': method}, {'value': 2}
            before, after = {'measurement_method': method}, {}
            if alternative is not None:
                right['measurement_method'] = alternative
                after['measurement_method'] = alternative
            with self.subTest(method=alternative), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(compare.report_conditions(before, after), ['method'])
                regression, output = self.compare(left, right)
                self.assertFalse(regression)
                self.assertIn('NOT COMPARABLE', output)
        regression, output = self.compare({'value': 1, 'measurement_method': method},
                                          {'value': 2, 'measurement_method': method})
        self.assertTrue(regression)
        self.assertNotIn('NOT COMPARABLE', output)


class MeasurementWindowTests(unittest.TestCase):
    def test_zero_one_burst_and_continuous_events_use_the_full_window(self):
        for stamps in ([], [0.2], [n / 20 for n in range(10)], [n * 1.4 for n in range(10)]):
            with self.subTest(stamps=stamps), tempfile.TemporaryDirectory() as directory, \
                    mock.patch.object(session.time, 'monotonic', side_effect=[0, 0, 15, 15]):
                path = pathlib.Path(directory) / 'diagnostics.log'
                path.write_text('# header\n')
                tail = session.DiagnosticTail(path)
                tail.mark()
                with path.open('a') as handle:
                    handle.writelines(f't={stamp} perf frame data\n' for stamp in stamps)
                window = tail.finish()
                self.assertEqual(window['counts'], {'frame': len(stamps), 'pass': 0})
                self.assertEqual(window['duration_seconds'], 15)
                self.assertEqual(window['counts']['frame'] / window['duration_seconds'], len(stamps) / 15)

    def test_visibility_boundaries_handle_delayed_and_incomplete_lines(self):
        for newline in (b'\n', b'\r\n'):
            with self.subTest(newline=newline), tempfile.TemporaryDirectory() as directory, \
                    mock.patch.object(session.time, 'monotonic', side_effect=[10, 10, 25, 25]):
                path = pathlib.Path(directory) / 'diagnostics.log'
                initial = b't=0 perf frame excluded' + newline
                path.write_bytes(initial + b't=1 perf frame partial')
                tail = session.DiagnosticTail(path)
                tail.mark()
                with path.open('ab') as handle:
                    handle.write(newline + b't=0.5 perf pass delayed' + newline + b't=20 perf frame incomplete')
                window = tail.finish()
                # Source timestamps do not define this observation window. A line
                # completed during it counts; one completed afterwards does not.
                with path.open('ab') as handle:
                    handle.write(newline + b't=21 perf pass arrived after the boundary' + newline)
                self.assertEqual(window['counts'], {'frame': 1, 'pass': 1})
                self.assertEqual(window['start_byte'], len(initial))
                self.assertEqual(window['duration_seconds'], 15)

    def test_a_log_created_after_start_is_allowed_but_missing_or_rotated_logs_are_not(self):
        for change in ('created', 'missing', 'rotated', 'rewritten'):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as directory, \
                    mock.patch.object(session.time, 'monotonic', side_effect=[0, 0, 15, 15]):
                path = pathlib.Path(directory) / 'diagnostics.log'
                if change in ('rotated', 'rewritten'):
                    path.write_text('# original header\n')
                tail = session.DiagnosticTail(path)
                tail.mark()
                if change == 'rotated':
                    path.rename(path.with_suffix('.previous'))
                if change != 'missing':
                    path.write_text('t=1 perf frame new\n')
                if change == 'created':
                    self.assertEqual(tail.finish()['counts']['frame'], 1)
                else:
                    with self.assertRaises(RuntimeError):
                        tail.finish()

    def test_an_append_after_the_file_size_snapshot_is_excluded(self):
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(session.time, 'monotonic', side_effect=[0, 0, 15, 15]):
            path = pathlib.Path(directory) / 'diagnostics.log'
            path.write_text('# header\n')
            tail = session.DiagnosticTail(path)
            tail.mark()
            path.write_text('# header\nt=1 perf frame inside\n')
            fstat = os.fstat

            def append_after_snapshot(fd):
                state = fstat(fd)
                with path.open('a') as handle:
                    handle.write('t=2 perf pass outside\n')
                return state

            with mock.patch.object(session.os, 'fstat', side_effect=append_after_snapshot):
                self.assertEqual(tail.finish()['counts'], {'frame': 1, 'pass': 0})

    def test_cpu_denominators_match_each_process_sample_interval(self):
        clock = [0.0]
        sleeps, calls = [], []
        costs = {1: iter([0.02, 0.1, 0.1, 0.02]), 2: iter([0.04, 0.08])}

        def sleep(seconds):
            sleeps.append(seconds)
            clock[0] += seconds

        def sample(pid):
            calls.append(pid)
            cost = next(costs[pid])
            midpoint = clock[0] + cost / 2
            clock[0] += cost
            final_app = pid == 1 and calls.count(1) == 4
            return types.SimpleNamespace(cpu_nanoseconds=midpoint * pid * 1e9,
                                         footprint_bytes=30 if final_app else 10,
                                         resident_bytes=40 if final_app else 20)

        class Tail:
            def mark(self):
                calls.append('mark')
                clock[0] += 0.02

            def finish(self):
                calls.append('finish')
                window = dict(session._window((0, 0), (clock[0], 0)), counts={'frame': 1, 'pass': 0})
                clock[0] += 3  # Reading/processing diagnostics cannot enter CPU numerators.
                return window

        with mock.patch.object(session.time, 'monotonic', side_effect=lambda: clock[0]), \
                mock.patch.object(session.time, 'sleep', side_effect=sleep), \
                mock.patch.object(session.quartz, 'process_sample', side_effect=sample, create=True):
            measured = session.measure(1, 2, 1, tail=Tail())
        self.assertAlmostEqual(measured.cores, 1)
        self.assertAlmostEqual(measured.content_cores, 2)
        self.assertEqual(measured.footprint_mb, 30 / 1e6)
        self.assertEqual(measured.resident_mb, 40 / 1e6)
        self.assertAlmostEqual(measured.windows['cpu']['duration_seconds'], 1.26)
        self.assertAlmostEqual(measured.windows['content-cpu']['duration_seconds'], 1.28)
        self.assertAlmostEqual(measured.windows['content-cpu']['end_sample_span_seconds'], 0.08)
        self.assertAlmostEqual(measured.frames_per_second, 1 / 1.38)
        self.assertEqual(calls, ['mark', 1, 2, 1, 1, 1, 2, 'finish'])
        self.assertEqual(sleeps, [0.5, 0.5])

    def test_method_and_raw_window_evidence_survive_flat_row_export(self):
        with mock.patch.dict(os.environ, {session.QUALITY_VARIABLE: '  standard  '}):
            method = session.measurement_method(True)
        self.assertEqual(method['quality_override'][session.QUALITY_VARIABLE], '  standard  ')
        with mock.patch.dict(os.environ, {session.QUALITY_VARIABLE: ''}):
            self.assertEqual(session.measurement_method(False)['quality_override'][session.QUALITY_VARIABLE], '')
        window = {'counts': {'frame': 1, 'pass': 0}, 'duration_seconds': 15}
        measured = session.Measurement(1, 2, 3, 1 / 15, 0, 0,
                                       windows={'diagnostics': window}, measurement_method=method)
        result = session.ScenarioResult(catalog.scenario_named('idle-region'), 'W', measured)
        rows = run._rows(result, dict(machine='m', os='o', build='b', version='v'))
        frame = next(row for row in rows if row['metric'].startswith('frames '))
        self.assertEqual(frame['window'], window)
        self.assertEqual(frame['measurement_method'], method)
        guard = types.SimpleNamespace(override='unused')
        self.assertEqual(run._environment(guard, pathlib.Path('diagnostics.log'))['SIDESCOPES_DIAG_FLUSH'], '1')
        self.assertEqual(run._environment(guard, None)['SIDESCOPES_DIAG'], '')


class PowerStateTests(unittest.TestCase):
    def test_charging_is_distinguished_from_discharging_and_not_charging(self):
        for status, charging in [('charging', True), ('discharging', False), ('not charging', False)]:
            with self.subTest(status=status), mock.patch.object(
                    conditions, '_command', return_value=f"Now drawing from 'AC Power'\n -InternalBattery-0 80%; {status}; 1:00 remaining"):
                self.assertEqual(conditions.power_state()['charging'], charging)


if __name__ == '__main__':
    unittest.main()
