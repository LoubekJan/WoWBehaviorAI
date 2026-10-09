"""Scope is receiver configuration; it never changes the telemetry wire schema."""
import copy
import json
import math
import unittest

from app.scope import contains_position, normalize_scope, scope_from_environment


LAB_SCOPE = {"map_id": 725, "zone_ids": [4988], "name": "AI World Lab",
             "bounds": {"min_x": 166.667, "max_x": 366.667, "min_y": 700, "max_y": 900}}


class ScopeTests(unittest.TestCase):
    def test_original_default_keeps_historical_map_only_behavior(self):
        scope = scope_from_environment({})
        self.assertEqual(scope, {"map_id": 0, "zone_ids": [12], "bounds": None, "name": "Elwynn Forest"})
        self.assertTrue(contains_position(scope, {"map_id": 0}))
        self.assertFalse(contains_position(scope, {"map_id": 725}))

    def test_lab_environment_and_edges_use_one_rectangle(self):
        env = {"WORLD_VIEWER_SCOPE_MAP_ID": "725", "WORLD_VIEWER_SCOPE_ZONE_IDS": "4988",
               "WORLD_VIEWER_SCOPE_NAME": "AI World Lab", "WORLD_VIEWER_SCOPE_BOUNDS": json.dumps(LAB_SCOPE['bounds'])}
        scope = scope_from_environment(env)
        self.assertEqual(scope, LAB_SCOPE)
        for x, y in ((266.667, 800), (166.667, 700), (366.667, 900)):
            self.assertTrue(contains_position(scope, {"map_id": 725, "x": x, "y": y}))
        for position in ({"map_id": 0, "x": 266.667, "y": 800}, {"map_id": 725, "x": 400, "y": 800},
                         {"map_id": 725, "x": 266.667}, {"map_id": 725, "x": math.nan, "y": 800}):
            self.assertFalse(contains_position(scope, position))

    def test_invalid_config_fails_instead_of_silently_using_elwynn(self):
        for change in ({"map_id": True}, {"map_id": -1}, {"zone_ids": []}, {"zone_ids": [4988, 4988]},
                       {"zone_ids": [True]}, {"name": ""}, {"bounds": {"min_x": 0}},
                       {"bounds": {"min_x": 10, "max_x": 0, "min_y": 0, "max_y": 10}},
                       {"bounds": {"min_x": 0, "max_x": math.inf, "min_y": 0, "max_y": 10}}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                normalize_scope({**copy.deepcopy(LAB_SCOPE), **change})
        for env in ({"WORLD_VIEWER_SCOPE_MAP_ID": "bad"}, {"WORLD_VIEWER_SCOPE_ZONE_IDS": ""},
                    {"WORLD_VIEWER_SCOPE_BOUNDS": "{"}):
            with self.subTest(env=env), self.assertRaises(ValueError):
                scope_from_environment(env)


if __name__ == "__main__":
    unittest.main()
