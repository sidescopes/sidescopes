"""Capability selection without launching or inspecting the desktop."""

import pathlib
import sys
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.scenarios import catalog


class CapabilityProfileTests(unittest.TestCase):
    def test_redraw_requires_a_real_clear_action(self):
        old_profile = next(profile for profile in catalog.PROFILES if profile.name == 'always-scoping')
        self.assertIn('clear-region', catalog.unavailable(
            catalog.scenario_named('region-redraw'), 'V', old_profile, 'V'))

    def test_continuity_profile_never_prices_cancellation_as_clearing(self):
        with mock.patch.object(catalog, 'strings_in', return_value=b'shortcut_cancel_interaction'):
            profile = catalog.detect_profile('candidate')
        self.assertEqual(profile.name, 'region-continuity')
        for identifier in ('idle-no-region', 'region-redraw'):
            self.assertIn('clear-region', catalog.unavailable(
                catalog.scenario_named(identifier), 'V', profile, 'V'))
        self.assertEqual(catalog.unavailable(
            catalog.scenario_named('region-replace'), 'V', profile, 'V'), '')

    def test_verified_clearable_profile_enables_empty_state_measurements(self):
        with mock.patch.object(catalog, 'strings_in', side_effect=AssertionError('explicit profile')):
            profile = catalog.detect_profile('candidate', 'region-clearable')
        for identifier in ('idle-no-region', 'region-redraw', 'region-replace'):
            self.assertEqual(catalog.unavailable(catalog.scenario_named(identifier), 'V', profile, 'V'), '')
        self.assertIn('retain-region', profile.capabilities)

    def test_unknown_override_cannot_fall_back_to_another_profile(self):
        self.assertIsNone(catalog.detect_profile('candidate', 'misspelled'))

    def test_each_unambiguous_legacy_marker_retains_its_profile(self):
        for name, marker in [('region-optional', b'shortcut_clear_region'),
                             ('always-scoping', b'shortcut_full_screen')]:
            with self.subTest(name=name), mock.patch.object(catalog, 'strings_in', return_value=marker):
                self.assertEqual(catalog.detect_profile('candidate').name, name)
