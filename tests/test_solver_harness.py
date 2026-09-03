import pathlib
import sys
import unittest


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))

from solver_harness import describe_difference, parse_k_spec  # noqa: E402


class HarnessTests(unittest.TestCase):
    def test_parse_k_spec_supports_ranges_and_values(self):
        self.assertEqual(parse_k_spec("1-3,5,3"), [1, 2, 3, 5])

    def test_parse_k_spec_rejects_out_of_reference_range(self):
        with self.assertRaises(ValueError):
            parse_k_spec("0,1")
        with self.assertRaises(ValueError):
            parse_k_spec("9")

    def test_difference_identifies_missing_and_extra_solutions(self):
        message = describe_difference({(1, "aaa")}, {(1, "bbb")})
        self.assertIn("1 missing, 1 extra", message)
        self.assertIn("missing k=1 sha256=aaa", message)
        self.assertIn("extra   k=1 sha256=bbb", message)


if __name__ == "__main__":
    unittest.main()
