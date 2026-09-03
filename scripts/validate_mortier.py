#!/usr/bin/env python3
"""Match generated Mortier records to ordinary Galebach catalogue names."""

import argparse
import json
import re
import sys
import math
from dataclasses import dataclass
from fractions import Fraction


GENERATED_NAME = re.compile(r"^k([0-9]+)_(.+)$")
ORDINARY_NAME = re.compile(r"^t([1-9][0-9]*)([0-9]{3})$")


@dataclass(frozen=True)
class Quadratic:
    """An exact value a + b*sqrt(3)."""

    rational: Fraction = Fraction(0)
    radical: Fraction = Fraction(0)

    def __add__(self, other):
        return Quadratic(self.rational + other.rational,
                         self.radical + other.radical)

    def __neg__(self):
        return Quadratic(-self.rational, -self.radical)

    def __sub__(self, other):
        return self + -other

    def __mul__(self, other):
        return Quadratic(
            self.rational * other.rational + 3 * self.radical * other.radical,
            self.rational * other.radical + self.radical * other.rational,
        )

    def __truediv__(self, other):
        denominator = other.rational * other.rational - 3 * other.radical * other.radical
        if denominator == 0:
            raise ZeroDivisionError
        return Quadratic(
            (self.rational * other.rational
             - 3 * self.radical * other.radical) / denominator,
            (self.radical * other.rational
             - self.rational * other.radical) / denominator,
        )

    def sign(self):
        if not self:
            return 0
        if self.rational == 0:
            return 1 if self.radical > 0 else -1
        if self.radical == 0 or self.rational * self.radical > 0:
            return 1 if self.rational > 0 else -1
        comparison = self.rational * self.rational - 3 * self.radical * self.radical
        if comparison == 0:  # Impossible for nonzero rational coefficients.
            raise ArithmeticError("unexpected rational representation of sqrt(3)")
        return (1 if self.rational > 0 else -1) * (1 if comparison > 0 else -1)

    def __bool__(self):
        return bool(self.rational or self.radical)


ZERO = Quadratic()
ONE = Quadratic(Fraction(1))
HALF = Fraction(1, 2)
Point = tuple[Quadratic, Quadratic]


@dataclass(frozen=True)
class Record:
    name: str
    k: int
    basis: tuple[Point, Point]
    seeds: tuple[Point, ...]

    @property
    def determinant(self):
        return determinant(*self.basis)


def z4_point(value) -> Point:
    if (not isinstance(value, list) or len(value) != 4
            or any(not isinstance(item, int) or isinstance(item, bool) for item in value)):
        raise ValueError(f"invalid Z4 point: {value!r}")
    a, b, c, d = value
    return (
        Quadratic(Fraction(2 * a + c, 2), Fraction(b, 2)),
        Quadratic(Fraction(b + 2 * d, 2), Fraction(c, 2)),
    )


def point_add(left: Point, right: Point) -> Point:
    return left[0] + right[0], left[1] + right[1]


def point_sub(left: Point, right: Point) -> Point:
    return left[0] - right[0], left[1] - right[1]


def determinant(left: Point, right: Point) -> Quadratic:
    return left[0] * right[1] - left[1] * right[0]


def absolute(value: Quadratic) -> Quadratic:
    return -value if value.sign() < 0 else value


def integer_ratio(numerator: Quadratic, denominator: Quadratic):
    ratio = numerator / denominator
    if ratio.radical or ratio.rational.denominator != 1:
        return None
    return int(ratio.rational)


def lattice_contains(point: Point, basis: tuple[Point, Point]) -> bool:
    denominator = determinant(*basis)
    if not denominator:
        return False
    return (integer_ratio(determinant(point, basis[1]), denominator) is not None
            and integer_ratio(determinant(basis[0], point), denominator) is not None)


def same_lattice(left: tuple[Point, Point], right: tuple[Point, Point]) -> bool:
    return (all(lattice_contains(generator, right) for generator in left)
            and all(lattice_contains(generator, left) for generator in right))


def residue(point: Point, basis: tuple[Point, Point]):
    denominator = determinant(*basis)
    first = determinant(point, basis[1]) / denominator
    second = determinant(basis[0], point) / denominator
    def modulo_one(value):
        approximate = float(value.rational) + math.sqrt(3) * float(value.radical)
        whole = math.floor(approximate)
        remainder = value - Quadratic(Fraction(whole))
        while remainder.sign() < 0:
            whole -= 1
            remainder = value - Quadratic(Fraction(whole))
        while (remainder - ONE).sign() >= 0:
            whole += 1
            remainder = value - Quadratic(Fraction(whole))
        return remainder
    return modulo_one(first), modulo_one(second)


# cos(n*pi/6), sin(n*pi/6), represented in Q(sqrt(3)).
ANGLES = (
    (ONE, ZERO),
    (Quadratic(ZERO.rational, HALF), Quadratic(HALF)),
    (Quadratic(HALF), Quadratic(ZERO.rational, HALF)),
    (ZERO, ONE),
    (Quadratic(-HALF), Quadratic(ZERO.rational, HALF)),
    (Quadratic(ZERO.rational, -HALF), Quadratic(HALF)),
    (-ONE, ZERO),
    (Quadratic(ZERO.rational, -HALF), Quadratic(-HALF)),
    (Quadratic(-HALF), Quadratic(ZERO.rational, -HALF)),
    (ZERO, -ONE),
    (Quadratic(HALF), Quadratic(ZERO.rational, -HALF)),
    (Quadratic(ZERO.rational, HALF), Quadratic(-HALF)),
)


def transform(point: Point, angle: int, reflected: bool) -> Point:
    x, y = point
    if reflected:
        y = -y
    cosine, sine = ANGLES[angle]
    return cosine * x - sine * y, sine * x + cosine * y


def equivalent(generated: Record, reference: Record) -> bool:
    if len(generated.seeds) != len(reference.seeds):
        return False
    if absolute(generated.determinant) != absolute(reference.determinant):
        return False
    reference_residues = frozenset(residue(seed, reference.basis)
                                   for seed in reference.seeds)
    for reflected in (False, True):
        for angle in range(12):
            basis = tuple(transform(vector, angle, reflected)
                          for vector in generated.basis)
            if not same_lattice(basis, reference.basis):
                continue
            seeds = tuple(transform(seed, angle, reflected)
                          for seed in generated.seeds)
            for generated_seed in seeds:
                for reference_seed in reference.seeds:
                    shift = point_sub(reference_seed, generated_seed)
                    shifted = frozenset(
                        residue(point_add(seed, shift), reference.basis)
                        for seed in seeds
                    )
                    if shifted == reference_residues:
                        return True
    return False


def parse_record(name, k, value) -> Record:
    if not isinstance(value, dict):
        raise ValueError(f"{name}: record must be an object")
    try:
        basis = (z4_point(value["T1"]), z4_point(value["T2"]))
        raw_seeds = value["Seed"]
    except KeyError as error:
        raise ValueError(f"{name}: missing {error.args[0]}") from None
    if not isinstance(raw_seeds, list) or not raw_seeds:
        raise ValueError(f"{name}: Seed must be a nonempty array")
    return Record(name, k, basis, tuple(z4_point(seed) for seed in raw_seeds))


def load_records(generated_data, reference_data, max_k=None):
    generated = []
    references = []
    singular = []
    for name, value in generated_data.items():
        match = GENERATED_NAME.fullmatch(name)
        if not match:
            continue
        k = int(match.group(1))
        if max_k is None or k <= max_k:
            generated.append(parse_record(name, k, value))
    for name, value in reference_data.items():
        match = ORDINARY_NAME.fullmatch(name)
        if not match:
            continue
        # The final three digits are always the catalogue index: t1003 is k=1,
        # index 003, not k=1003. This also remains unambiguous for k >= 10.
        k = int(match.group(1))
        if max_k is not None and k > max_k:
            continue
        record = parse_record(name, k, value)
        if not record.determinant:
            singular.append(name)
        else:
            references.append(record)
    return generated, references, singular


def match_records(generated, references):
    index = {}
    for reference in references:
        key = (reference.k, len(reference.seeds), absolute(reference.determinant))
        index.setdefault(key, []).append(reference)
    matches = {}
    unmatched = []
    ambiguous = {}
    for record in sorted(generated, key=lambda item: item.name):
        if not record.determinant:
            unmatched.append(record.name)
            continue
        key = (record.k, len(record.seeds), absolute(record.determinant))
        names = [candidate.name for candidate in index.get(key, ())
                 if equivalent(record, candidate)]
        if len(names) == 1:
            matches[record.name] = names[0]
        elif names:
            ambiguous[record.name] = sorted(names)
        else:
            unmatched.append(record.name)
    return matches, unmatched, ambiguous


def stable_id(name):
    match = GENERATED_NAME.fullmatch(name)
    return match.group(2) if match else name


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated", required=True, help="exporter JSON")
    parser.add_argument("--reference", required=True, help="Mortier database.json")
    parser.add_argument("--max-k", type=int, help="only compare entries through this k")
    args = parser.parse_args(argv)
    if args.max_k is not None and args.max_k < 1:
        parser.error("--max-k must be positive")
    try:
        with open(args.generated, encoding="utf-8") as stream:
            generated_data = json.load(stream)
        with open(args.reference, encoding="utf-8") as stream:
            reference_data = json.load(stream)
        if not isinstance(generated_data, dict) or not isinstance(reference_data, dict):
            raise ValueError("both JSON roots must be objects")
        generated, references, singular = load_records(
            generated_data, reference_data, args.max_k)
        matches, unmatched, ambiguous = match_records(generated, references)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        parser.exit(2, f"error: {error}\n")

    print("matched:")
    for name in sorted(matches):
        print(f"  {stable_id(name)} -> {matches[name]}")
    print("unmatched:")
    for name in unmatched:
        print(f"  {stable_id(name)}")
    print("ambiguous:")
    for name in sorted(ambiguous):
        print(f"  {stable_id(name)} -> {', '.join(ambiguous[name])}")
    print("singular reference:")
    for name in sorted(singular):
        print(f"  {name}")
    return 1 if unmatched or ambiguous else 0


if __name__ == "__main__":
    sys.exit(main())
