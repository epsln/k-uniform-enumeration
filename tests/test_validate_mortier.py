import pathlib
import sys
import unittest


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))

from validate_mortier import load_records, match_records  # noqa: E402


X = [1, 0, 0, 0]
Y = [0, 0, 0, 1]


def record(t1, t2, seeds):
    return {"T1": t1, "T2": t2, "Seed": seeds}


def matched(generated_value, reference_value):
    generated, references, singular = load_records(
        {"k01_deadbeef": generated_value}, {"t1003": reference_value})
    matches, unmatched, ambiguous = match_records(generated, references)
    return matches, unmatched, ambiguous, singular


class ValidateMortierTests(unittest.TestCase):
    def test_basis_swap_and_shear(self):
        generated = record([2, 0, 0, 0], [0, 0, 0, 2], [[0, 0, 0, 0]])
        reference = record([0, 0, 0, 2], [2, 0, 0, 2], [[0, 0, 0, 0]])
        matches, unmatched, ambiguous, _ = matched(generated, reference)
        self.assertEqual(matches, {"k01_deadbeef": "t1003"})
        self.assertEqual((unmatched, ambiguous), ([], {}))

    def test_common_translation_and_residue_representatives(self):
        generated = record(
            [3, 0, 0, 0], [0, 0, 0, 3],
            [[1, 0, 0, 0], [2, 0, 0, 1]],
        )
        reference = record(
            [3, 0, 0, 0], [0, 0, 0, 3],
            [[6, 0, 0, 5], [5, 0, 0, 4]],
        )
        self.assertIn("k01_deadbeef", matched(generated, reference)[0])

    def test_rotation_and_reflection(self):
        # Reflection in the x axis, followed by a 30 degree rotation.
        generated = record(
            [2, 0, 0, 0], [0, 0, 0, 3],
            [[0, 0, 0, 0], [1, 0, 0, 1]],
        )
        reference = record(
            [0, 2, 0, 0], [3, 0, -3, 0],
            [[0, 0, 0, 0], [1, 1, -1, 0]],
        )
        self.assertIn("k01_deadbeef", matched(generated, reference)[0])

    def test_same_area_non_equivalent_lattice_is_unmatched(self):
        generated = record([2, 0, 0, 0], [0, 0, 0, 1], [[0, 0, 0, 0]])
        reference = record([1, 0, 0, 1], [0, 0, 0, 2], [[0, 0, 0, 0]])
        matches, unmatched, ambiguous, _ = matched(generated, reference)
        self.assertEqual(matches, {})
        self.assertEqual(unmatched, ["k01_deadbeef"])
        self.assertEqual(ambiguous, {})

    def test_singular_reference_is_reported_and_not_compared(self):
        generated = {"k01_deadbeef": record(X, Y, [[0, 0, 0, 0]])}
        reference = {
            "t1003": record(X, [2, 0, 0, 0], [[0, 0, 0, 0]]),
            "not-an-ordinary-name": record(X, Y, [[0, 0, 0, 0]]),
        }
        records, references, singular = load_records(generated, reference)
        self.assertEqual(singular, ["t1003"])
        self.assertEqual(references, [])
        self.assertEqual(match_records(records, references)[1], ["k01_deadbeef"])


if __name__ == "__main__":
    unittest.main()
