import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(__file__))

import mb_source


class SourcePipelineTests(unittest.TestCase):
    def test_udp_pipeline_has_large_receive_buffer_and_discards_bad_frames(self):
        desc = mb_source.udp(15004)
        self.assertIn("udpsrc port=15004 buffer-size=8388608", desc)
        self.assertIn("mppvideodec discard-corrupted-frames=true", desc)
        self.assertIn("queue max-size-buffers=3", desc)
        self.assertIn("leaky=downstream", desc)
        self.assertIn("gtk4paintablesink name=sink sync=false", desc)

    def test_clip_playback_stays_clock_synced_without_leaky_queue(self):
        desc = mb_source.clip("http://127.0.0.1/events/test.mp4")
        self.assertIn("gtk4paintablesink name=sink sync=true", desc)
        self.assertNotIn("leaky=downstream", desc)

    def test_udp_drop_counter_parser(self):
        sample = (
            "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when "
            "retrnsmt   uid  timeout inode ref pointer drops\n"
            " 1283: 00000000:3A9C 00000000:0000 07 00000000:00000000 00:00000000 "
            "00000000  1001        0 290518 2 0000000065135073 589\n"
        )
        self.assertEqual(mb_source._udp_drops_from(sample, 15004), 589)
        self.assertIsNone(mb_source._udp_drops_from(sample, 15005))


if __name__ == "__main__":
    unittest.main()
