#include "mortier_geometry.h"
#include "mortier_store.h"
#include "bfl.h"
#include "solver.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace mortier_geometry;

bool g_propagate = true;
bool g_binary_solutions = false;
bool g_no_spill = false;

static void test_codec() {
    MortierRecord record{
        Z4Point{{1, -2, 3, -4}}, Z4Point{{-5, 6, -7, 8}},
        {Z4Point{{2, 0, 0, 0}}, Z4Point{}, Z4Point{{1, 1, 0, 0}}}
    };
    auto encoded = encode_mortier_record(record);
    MortierRecord decoded = decode_mortier_record(encoded);
    assert(decoded.t1.coordinates == record.t1.coordinates);
    assert(decoded.t2.coordinates == record.t2.coordinates);
    assert(decoded.seeds.size() == 3);
    assert(decoded.seeds[0].coordinates == Z4Point{}.coordinates);

    encoded.pop_back();
    bool rejected = false;
    try { (void)decode_mortier_record(encoded); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
}

static void test_stable_hash_encoding() {
    std::string hash = bfl_canonical_hash(EuclideanSolver::make_initial(0));
    const std::string expected =
        "8f9e3b66cd530bb42e0fd8c91f3d4d70942b11e52933f27bfb49408e956a94de";
    assert(format_hex_id(std::vector<uint8_t>(hash.begin(), hash.end())) == expected);
}

static void test_regular_tilings() {
    for (int sides : {3, 4, 6}) {
        auto description = make_tiling_description({sides}, {sides}, "(0)");
        DevelopmentResult result = develop_exact_geometry(description);
        assert(!result.record.seeds.empty());
        validate_mortier_record(result.record);
    }

    DevelopmentResult mirrored = develop_exact_geometry(
        make_tiling_description({4}, {4}, "[0]"));
    validate_mortier_record(mirrored.record);
}

static void test_store_and_json() {
    fs::path root = fs::temp_directory_path() / "eusolver-mortier-test";
    fs::remove_all(root);
    fs::create_directories(root);
    fs::path database = root / "tilings.sqlite3";
    fs::path json = root / "tiling.json";
    MortierRecord record = develop_exact_geometry(
        make_tiling_description({4}, {4}, "(0)")).record;
    std::vector<uint8_t> stable_id(32, 0);
    stable_id[0] = 0x01;
    stable_id[1] = 0xab;
    stable_id[2] = 0xff;
    {
        MortierStore store(database.string(), 32);
        store.add(stable_id, 1, record, std::string("square"));
        store.finish();
    }
    assert(MortierStore::is_complete(database.string()));
    auto stored = MortierStore::read_by_stable_id(database.string(), stable_id);
    assert(stored && stored->k == 1 && stored->record.seeds == record.seeds);
    assert(export_mortier_json(database.string(), json.string(), stored->id) == 1);
    bool same_path_rejected = false;
    try { (void)export_mortier_json(database.string(), database.string()); }
    catch (const std::invalid_argument&) { same_path_rejected = true; }
    assert(same_path_rejected);
    fs::path database_link = root / "database-link.sqlite3";
    fs::create_symlink(database.filename(), database_link);
    bool alias_rejected = false;
    try { (void)export_mortier_json(database_link.string(), database.string()); }
    catch (const std::invalid_argument&) { alias_rejected = true; }
    assert(alias_rejected);
    std::ifstream input(json);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    assert(text.find("k01_01abff0000000000000000000000000000000000000000000000000000000000")
           != std::string::npos);
    assert(text.find("\"Seed\"") != std::string::npos);
    fs::path imported = root / "imported.sqlite3";
    assert(import_mortier_json(json.string(), imported.string(), 32) == 1);
    auto round_trip = MortierStore::read_by_stable_id(imported.string(), stable_id);
    assert(round_trip && encode_mortier_record(round_trip->record)
                         == encode_mortier_record(record));

    fs::path invalid_json = root / "invalid.json";
    fs::path invalid_database = root / "invalid.sqlite3";
    {
        std::ofstream invalid(invalid_json);
        invalid << "{\"k01_" << std::string(64, '0')
                << "\":{\"T1\":[1,0,0,0],\"T2\":[2,0,0,0],"
                   "\"Seed\":[[0,0,0,0]]}}";
    }
    bool invalid_rejected = false;
    try { (void)import_mortier_json(invalid_json.string(), invalid_database.string()); }
    catch (const std::invalid_argument&) { invalid_rejected = true; }
    assert(invalid_rejected && !fs::exists(invalid_database));
    fs::remove_all(root);
}

int main() {
    test_codec();
    test_stable_hash_encoding();
    test_regular_tilings();
    test_store_and_json();
}
