from __future__ import annotations

import unittest

import esphome.config_validation as cv

from components.quietcool import validate_heartbeat_interval


class HeartbeatSchemaTest(unittest.TestCase):
    def test_disabled_and_enabled_bounds_are_exact(self) -> None:
        for value, seconds in (("0s", 0), ("60s", 60), ("3600s", 3600)):
            with self.subTest(value=value):
                self.assertEqual(
                    validate_heartbeat_interval(value).total_seconds, seconds
                )

        for value in ("-1s", "1s", "59s", "3601s", "60.5s"):
            with self.subTest(value=value):
                with self.assertRaises(cv.Invalid):
                    validate_heartbeat_interval(value)


if __name__ == "__main__":
    unittest.main()
