import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import display_audio_common as common


class ConfigurationTests(unittest.TestCase):
    def profile(self, profile_id="lg", serial="ABC", sink="alsa.hdmi"):
        return {
            "id": profile_id,
            "label": "LG Display",
            "serial": serial,
            "bus": 4,
            "sink": sink,
            "enabled": True,
            "minimum": 5,
            "maximum": 80,
            "curve": 1.5,
            "mute": "auto",
        }

    def test_round_trip_and_atomic_save(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.dict(os.environ, {"XDG_CONFIG_HOME": directory}):
                config = common.new_config()
                common.write_profile(config, self.profile())
                common.save_config(config)
                loaded = common.load_config()
                self.assertEqual(common.profiles(loaded)[0]["curve"], 1.5)
                self.assertEqual(
                    Path(directory, "display-audio", "config.ini").stat().st_mode
                    & 0o777,
                    0o600,
                )

    def test_duplicate_hardware_is_rejected(self):
        config = common.new_config()
        common.write_profile(config, self.profile())
        common.write_profile(
            config, self.profile(profile_id="other", sink="alsa.other")
        )
        with self.assertRaisesRegex(ValueError, "serial"):
            common.validate_config(config)

    def test_unique_remainder_is_only_automatic_pair(self):
        config = common.new_config()
        with mock.patch.object(
            common,
            "discover_displays",
            return_value=[{"serial": "ABC", "bus": 4, "label": "LG"}],
        ), mock.patch.object(
            common,
            "discover_sinks",
            return_value=[{"name": "alsa_output.card.hdmi", "label": "HDMI"}],
        ):
            self.assertIsNotNone(common.confident_unassigned_pair(config))

    def test_ambiguous_pair_is_not_automatic(self):
        config = common.new_config()
        with mock.patch.object(
            common,
            "discover_displays",
            return_value=[
                {"serial": "ABC", "bus": 4, "label": "LG"},
                {"serial": "DEF", "bus": 5, "label": "Dell"},
            ],
        ), mock.patch.object(
            common,
            "discover_sinks",
            return_value=[{"name": "alsa_output.card.hdmi", "label": "HDMI"}],
        ):
            self.assertIsNone(common.confident_unassigned_pair(config))

    def test_ddcutil_identity_parsing(self):
        output = """Display 1
   I2C bus:          /dev/i2c-4
   Monitor:          GSM:LG ULTRAGEAR+:412NTVS1P487
"""
        with mock.patch.object(common, "command_output", return_value=output):
            self.assertEqual(
                common.discover_displays(),
                [
                    {
                        "bus": 4,
                        "serial": "412NTVS1P487",
                        "label": "GSM LG ULTRAGEAR+",
                    }
                ],
            )


if __name__ == "__main__":
    unittest.main()
