#include "mortier_geometry.h"
#include "mortier_store.h"
#include "bfl.h"
#include "solver.h"
#include "tes_store.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <sqlite3.h>

namespace fs = std::filesystem;
using namespace mortier_geometry;

bool g_propagate = true;
bool g_binary_solutions = false;
bool g_no_spill = false;

static int64_t scalar(const fs::path& database, const char* sql);

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

static void test_finite_voltage_development() {
    // Database fixture 13_34 / (3,3,3,4,4)Ax3,
    // (3,3,3,3,3,3)A2x10 / eu 5c3 6i10 61827.tes.
    const std::string document =
        "## Euclidean, high-k finite-voltage fixture\n"
        " e2. angleunit( deg )\n"
        "unittile(90,90,90,90)\n"
        "unittile(60,60,60)\nunittile(60,60,60)\n"
        "unittile(60,60,60)\nunittile(60,60,60)\n"
        "unittile(60,60,60)\nunittile(60,60,60)\n"
        "unittile(60,60,60)\nunittile(60,60,60)\n"
        "unittile(60,60,60)\nunittile(60,60,60)\n"
        "unittile(60,60,60)\nunittile(60,60,60)\n"
        "unittile(60,60,60)\nunittile(60,60,60)\n"
        "unittile(60,60,60)\nunittile(60,60,60)\n"
        "unittile(60,60,60)\nunittile(60,60,60)\n"
        "unittile(60,60,60)\nunittile(60,60,60)\n"
        "unittile(60,60,60)\nunittile(60,60,60)\n"
        "unittile(60,60,60)\n"
        "unittile(90,90,90,90)\n"
        "conway(\""
        "(0 0')(1 3)(2 0@4)(1' 0'')(2' 1'')(2'' 1''')(0''')(2''')"
        "(1@4 0@5)(2@4 1@5)(2@5 1@6)(0@6 1@7)(2@6 0@7)"
        "(2@7 0@8)(1@8 0@9)(2@8 1@9)(2@9 1@10)(0@10 1@11)"
        "(2@10 0@11)(2@11 0@12)(1@12 0@13)(2@12 1@13)"
        "(2@13 1@14)(0@14 1@15)(2@14 0@15)(2@15 0@16)"
        "(1@16 0@17)(2@16 1@17)(2@17 1@18)(0@18 1@19)"
        "(2@18 0@19)(2@19 0@20)(1@20 0@21)(2@20 1@21)"
        "(2@21 1@22)(0@22 1@23)(2@22 0@23)(2@23 0@24)(1@24)"
        "\")\nrepeat(24,2) ## repeated square pattern\n";
    auto description = parse_generated_tes(document);
    assert(description.polygon_sides.size() == 25);
    assert(description.polygon_sides.front() == 4);
    assert(description.polygon_sides.back() == 4);
    assert(description.repeats.back() == 2);
    DevelopmentOptions obsolete_limits;
    obsolete_limits.max_tiles = 0;
    obsolete_limits.max_depth = 0;
    obsolete_limits.max_quotient_tiles = 0;
    DevelopmentResult result = develop_exact_geometry(description, obsolete_limits);
    assert(result.developed_tiles <= 2 * 12 * description.polygon_sides.size());
    assert(result.quotient_tiles == result.developed_tiles);
    assert(result.translation_candidates >= 2);
    validate_mortier_record(result.record);
}

static void test_generated_tes_rejections() {
    const std::vector<std::pair<int, std::string>> regular_tiles{
        {3, "60,60,60"},
        {4, "90,90,90,90"},
        {6, "120,120,120,120,120,120"},
        {12, "150,150,150,150,150,150,150,150,150,150,150,150"}
    };
    for (const auto& tile : regular_tiles) {
        std::string document = "e2. angleunit(deg) unittile(" + tile.second
            + ") conway(\"(0)\") repeat(0," + std::to_string(tile.first) + ")";
        assert(parse_generated_tes(document).polygon_sides == std::vector<int>{tile.first});
    }
    auto rejected = [](const std::string& document) {
        try {
            (void)parse_generated_tes(document);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };
    assert(rejected("e2. unittile(90,90,90,90) bogus(x) conway(\"(0)\")"));
    assert(rejected("e2. unittile(90,90,90,90) conway(\"(0)\")"));
    assert(rejected("e2 unittile(90,90,90,90) conway(\"(0)\")"));
    assert(rejected("e2. unittile(90,90,89,90) conway(\"(0)\")"));
    assert(rejected("e2. unittile(108,108,108,108,108) conway(\"(0)\")"));
    assert(rejected("e2. unittile(90,90,90,90) conway(\"(0)\") repeat(0,2) repeat(0,2)"));
    assert(rejected("e2. unittile(90,90,90,90) conway(\"(0)\") repeat(1,1)"));
    assert(rejected("unittile(90,90,90,90) conway(\"(0)\")"));
    assert(rejected("e2. conway(\"(0)\")"));
    assert(rejected("e2. unittile(90,90,90,90)"));
    assert(rejected("e2. angleunit(rad) unittile(90,90,90,90) conway(\"(0)\")"));
    assert(rejected("e2. angleunit(deg) unittile(90,90,90,90) "
                    "conway(\"(999999999999999999999)\")"));
    assert(rejected("e2. unittile(90,90,90,90) conway(\"(1)\")"));
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

    fs::path tes_database = root / "tes.sqlite3";
    fs::path wrong_json = root / "wrong.json";
    {
        TesStore store(tes_database.string());
        store.add("01_4", "(4,4,4,4)F", "square.tes", "e2.\n");
        store.finish();
    }
    bool tes_rejected = false;
    try {
        (void)export_mortier_json(tes_database.string(), wrong_json.string());
    } catch (const std::runtime_error& error) {
        tes_rejected = std::string(error.what()).find("TES database") != std::string::npos;
    }
    assert(tes_rejected);
    assert(!fs::exists(wrong_json.string() + ".tmp"));

    fs::path legacy_json = root / "legacy.json";
    fs::path legacy_database = root / "legacy.sqlite3";
    MortierRecord square = develop_exact_geometry(
        make_tiling_description({4}, {4}, "(0)")).record;
    auto write_point = [](std::ostream& output, const Z4Point& point) {
        output << '[' << point.coordinates[0] << ',' << point.coordinates[1] << ','
               << point.coordinates[2] << ',' << point.coordinates[3] << ']';
    };
    {
        std::ofstream legacy(legacy_json);
        auto write_record = [&](const std::string& name) {
            legacy << '"' << name << "\":{\"T1\":";
            write_point(legacy, square.t1);
            legacy << ",\"T2\":";
            write_point(legacy, square.t2);
            legacy << ",\"Seed\":[";
            for (size_t i = 0; i < square.seeds.size(); ++i) {
                if (i) legacy << ',';
                write_point(legacy, square.seeds[i]);
            }
            legacy << "]}";
        };
        legacy << '{';
        write_record("eu_raw_4u_1");
        legacy << ',';
        write_record("eu_4u_5d2_6h_7");
        legacy << ",\"_failures\":{\"eu_raw_6c_1\":\"Only 3 vertices placed\"}}";
    }
    bool legacy_incomplete = false;
    try {
        (void)import_mortier_json(legacy_json.string(), legacy_database.string(), 32);
    } catch (const std::runtime_error& error) {
        legacy_incomplete = std::string(error.what()).find("failed source record")
            != std::string::npos;
    }
    assert(legacy_incomplete && fs::exists(legacy_database));
    assert(!MortierStore::is_complete(legacy_database.string()));
    assert(scalar(legacy_database, "SELECT COUNT(*) FROM import_failures;") == 1);
    bool incomplete_export_rejected = false;
    try { (void)export_mortier_json(legacy_database.string(), (root / "incomplete.json").string()); }
    catch (const std::runtime_error&) { incomplete_export_rejected = true; }
    assert(incomplete_export_rejected);
    std::vector<MortierStoredEntry> legacy_entries;
    MortierStore::for_each(legacy_database.string(),
                           [&](const MortierStoredEntry& entry) { legacy_entries.push_back(entry); });
    assert(legacy_entries.size() == 2);
    assert(legacy_entries[0].k == 1 && legacy_entries[0].signature == "eu_raw_4u_1");
    assert(legacy_entries[1].k == 4 && legacy_entries[1].signature == "eu_4u_5d2_6h_7");
    assert(legacy_entries[0].stable_id.size() == 32);
    assert(legacy_entries[0].stable_id != legacy_entries[1].stable_id);

    fs::path duplicate_residue = root / "duplicate-residue.json";
    fs::path duplicate_database = root / "duplicate-residue.sqlite3";
    {
        std::ofstream duplicate(duplicate_residue);
        duplicate << "{\"k01_" << std::string(64, '1')
                  << "\":{\"T1\":[1,0,0,0],\"T2\":[0,0,0,1],"
                     "\"Seed\":[[0,0,0,0],[1,0,0,0]]}}";
    }
    bool residue_rejected = false;
    try { (void)import_mortier_json(duplicate_residue.string(), duplicate_database.string()); }
    catch (const std::runtime_error&) { residue_rejected = true; }
    assert(residue_rejected && !fs::exists(duplicate_database));

    auto legacy_rejected = [&](const std::string& contents, const std::string& stem) {
        fs::path input = root / (stem + ".json");
        fs::path output = root / (stem + ".sqlite3");
        std::ofstream stream(input);
        stream << contents;
        stream.close();
        try {
            (void)import_mortier_json(input.string(), output.string(), 32);
            return false;
        } catch (const std::runtime_error&) {
            return !fs::exists(output) && !fs::exists(output.string() + ".tmp");
        }
    };
    assert(legacy_rejected("{\"eu_raw_bad_1\":{\"T1\":[1,0,0,0],"
                           "\"T2\":[0,0,0,1],\"Seed\":[[0,0,0,0]]}}",
                           "bad-name"));
    assert(legacy_rejected("{\"_failures\":[\"not an object\"]}", "bad-failures"));
    assert(legacy_rejected("{\"_failures\":{\"eu_raw_6c_1\":42}}",
                           "bad-failure-message"));
    fs::remove_all(root);
}

static std::string regular_tes(int sides) {
    std::string angles;
    int angle = 180 - 360 / sides;
    for (int i = 0; i < sides; ++i) {
        if (i) angles += ',';
        angles += std::to_string(angle);
    }
    return "e2. angleunit(deg) unittile(" + angles
        + ") conway(\"(0)\") repeat(0," + std::to_string(sides) + ")";
}

static int64_t scalar(const fs::path& database, const char* sql) {
    sqlite3* db = nullptr;
    assert(sqlite3_open_v2(database.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    assert(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    int64_t result = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return result;
}

static void execute(const fs::path& database, const std::string& sql) {
    sqlite3* db = nullptr;
    assert(sqlite3_open_v2(database.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr)
           == SQLITE_OK);
    assert(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
}

static void test_schema_one_compatibility() {
    fs::path root = fs::temp_directory_path() / "eusolver-mortier-schema-test";
    fs::remove_all(root);
    fs::create_directories(root);
    fs::path database = root / "schema-one.sqlite3";
    MortierRecord record = develop_exact_geometry(
        make_tiling_description({4}, {4}, "(0)")).record;
    {
        MortierStore store(database.string(), 32);
        store.add(std::vector<uint8_t>(32, 1), 1, record);
        store.finish();
    }
    assert(scalar(database,
        "SELECT value FROM metadata WHERE key='schema_version';") == 2);
    execute(database,
        "UPDATE metadata SET value='1' WHERE key='schema_version';"
        "CREATE TABLE entries_v1("
        " id INTEGER PRIMARY KEY,stable_id BLOB NOT NULL UNIQUE,k INTEGER NOT NULL CHECK(k>=0),"
        " chunk_id INTEGER NOT NULL REFERENCES chunks(id),"
        " byte_offset INTEGER NOT NULL CHECK(byte_offset>=0),"
        " byte_length INTEGER NOT NULL CHECK(byte_length>0),"
        " seed_count INTEGER NOT NULL CHECK(seed_count>=0),signature TEXT);"
        "INSERT INTO entries_v1 SELECT id,stable_id,k,chunk_id,byte_offset,byte_length,"
        "seed_count,signature FROM entries;"
        "DROP TABLE entries;ALTER TABLE entries_v1 RENAME TO entries;");
    auto stored = MortierStore::read_by_id(database.string(), 1);
    assert(stored && stored->record.seeds == record.seeds);
    fs::remove_all(root);
}

static void test_tes_fingerprint() {
    fs::path root = fs::temp_directory_path() / "eusolver-tes-fingerprint-test";
    fs::remove_all(root);
    fs::create_directories(root);
    const std::vector<TesEntry> entries{
        {0, "01_3", "alpha", "first.tes", "first document"},
        {0, "02_4", "middle", "middle.tes", std::string("mid\0document", 12)},
        {0, "03_6", "omega", "last.tes", "last document"}
    };
    auto write = [&](const fs::path& path, size_t chunk_target, bool mutate_middle) {
        TesStore store(path.string(), chunk_target);
        for (size_t i = 0; i < entries.size(); ++i) {
            TesEntry entry = entries[i];
            if (mutate_middle && i == 1) entry.document[4] = 'X';
            store.add(entry.combo, entry.signature, entry.legacy_filename, entry.document);
        }
        store.finish();
    };

    fs::path one_per_chunk = root / "one-per-chunk.sqlite3";
    fs::path single_chunk = root / "single-chunk.sqlite3";
    fs::path mutated = root / "mutated.sqlite3";
    write(one_per_chunk, 1, false);
    write(single_chunk, 1024 * 1024, false);
    write(mutated, 1024 * 1024, true);

    const std::string first = TesReader(one_per_chunk.string()).identity().fingerprint;
    const std::string second = TesReader(single_chunk.string()).identity().fingerprint;
    const std::string changed = TesReader(mutated.string()).identity().fingerprint;
    assert(first == second);
    assert(first == "a66371bdea7486ed9919826ecaedca94a28a601d1aaa7467d4f86be5dd4ec0ae");
    assert(changed != first);
    fs::remove_all(root);
}

static void test_tes_conversion() {
    fs::path root = fs::temp_directory_path() / "eusolver-tes-conversion-test";
    fs::remove_all(root);
    fs::create_directories(root);
    fs::path source = root / "source.sqlite3";
    {
        TesStore store(source.string(), 1);
        store.add("01_3", "triangular", "triangle.tes", regular_tes(3));
        store.add("02_4", "square", "square.tes", regular_tes(4));
        store.finish();
    }
    TesReader reader(source.string());
    std::vector<int64_t> source_ids;
    reader.for_each_after(0, [&](const TesEntry& entry) {
        source_ids.push_back(entry.id);
        assert(!entry.document.empty());
    });
    assert((source_ids == std::vector<int64_t>{1, 2}));
    fs::path destination = root / "mortier.sqlite3";
    auto result = convert_tes_to_mortier(source.string(), destination.string(), false,
                                         ConversionErrorMode::Fail, 1);
    assert(result.complete && result.converted_count == 2 && result.failure_count == 0);
    assert(!fs::exists(destination.string() + ".partial"));
    std::vector<MortierStoredEntry> entries;
    MortierStore::for_each(destination.string(),
                           [&](const MortierStoredEntry& entry) { entries.push_back(entry); });
    assert(entries.size() == 2);
    assert(entries[0].source_entry_id == 1 && entries[0].k == 1
           && entries[0].signature == "triangular");
    assert(entries[1].source_entry_id == 2 && entries[1].k == 2
           && entries[1].signature == "square");

    fs::path copied_source = root / "copied.sqlite3";
    fs::copy_file(source, copied_source);
    fs::path copied_destination = root / "copied-mortier.sqlite3";
    assert(convert_tes_to_mortier(copied_source.string(), copied_destination.string()).complete);
    auto copied = MortierStore::read_by_id(copied_destination.string(), 1);
    assert(copied && copied->stable_id == entries[0].stable_id);

    fs::path invalid_source = root / "invalid-source.sqlite3";
    {
        TesStore store(invalid_source.string(), 1);
        store.add("01_4", "valid", "valid.tes", regular_tes(4));
        store.add("02_4", "invalid", "invalid.tes", "not TES");
        store.finish();
    }
    fs::path failed_destination = root / "failed.sqlite3";
    bool failed = false;
    try {
        (void)convert_tes_to_mortier(invalid_source.string(), failed_destination.string());
    } catch (const std::runtime_error&) {
        failed = true;
    }
    assert(failed && !fs::exists(failed_destination));
    fs::path partial = failed_destination.string() + ".partial";
    assert(fs::exists(partial));
    assert(scalar(partial, "SELECT last_source_id FROM conversion_state;") == 1);
    assert(scalar(partial, "SELECT converted_count FROM conversion_state;") == 1);
    assert(scalar(partial, "SELECT failure_count FROM conversion_state;") == 1);
    bool resume_failed = false;
    try {
        (void)convert_tes_to_mortier(invalid_source.string(), failed_destination.string(), true);
    } catch (const std::runtime_error&) {
        resume_failed = true;
    }
    assert(resume_failed);
    assert(scalar(partial, "SELECT COUNT(*) FROM entries;") == 1);

    fs::path continued_destination = root / "continued.sqlite3";
    auto continued = convert_tes_to_mortier(
        invalid_source.string(), continued_destination.string(), false,
        ConversionErrorMode::Continue, 1);
    assert(!continued.complete && continued.converted_count == 1
           && continued.failure_count == 1);
    assert(!fs::exists(continued_destination));
    assert(fs::exists(continued_destination.string() + ".partial"));

    fs::path retry_source = root / "retry-source.sqlite3";
    {
        TesStore store(retry_source.string(), 1);
        store.add("01_3", "first", "first.tes", regular_tes(3));
        store.add("02_4", "retry", "retry.tes", regular_tes(4));
        store.add("03_6", "last", "last.tes", regular_tes(6));
        store.finish();
    }
    fs::path retry_destination = root / "retry.sqlite3";
    assert(convert_tes_to_mortier(retry_source.string(), retry_destination.string()).complete);
    fs::path retry_partial = retry_destination.string() + ".partial";
    fs::rename(retry_destination, retry_partial);
    execute(retry_partial,
        "UPDATE metadata SET value='0' WHERE key='complete';"
        "DELETE FROM entries WHERE source_entry_id=2;"
        "INSERT INTO conversion_failures VALUES(2,'simulated transient failure');"
        "UPDATE conversion_state SET converted_count=2,failure_count=1;");
    bool corrupt_resume_rejected = false;
    try {
        (void)convert_tes_to_mortier(
            retry_source.string(), retry_destination.string(), true,
            ConversionErrorMode::Continue, 1);
    } catch (const std::runtime_error&) {
        corrupt_resume_rejected = true;
    }
    assert(corrupt_resume_rejected && fs::exists(retry_partial));

    fs::path corrupt_chunk_destination = root / "corrupt-chunk.sqlite3";
    assert(convert_tes_to_mortier(retry_source.string(), corrupt_chunk_destination.string()).complete);
    fs::path corrupt_chunk_partial = corrupt_chunk_destination.string() + ".partial";
    fs::rename(corrupt_chunk_destination, corrupt_chunk_partial);
    execute(corrupt_chunk_partial,
        "UPDATE metadata SET value='0' WHERE key='complete';"
        "UPDATE chunks SET data=zeroblob(length(data));");
    bool corrupt_chunk_rejected = false;
    try {
        (void)convert_tes_to_mortier(retry_source.string(),
                                     corrupt_chunk_destination.string(), true);
    } catch (const std::runtime_error&) {
        corrupt_chunk_rejected = true;
    }
    assert(corrupt_chunk_rejected && fs::exists(corrupt_chunk_partial));

    bool alias_rejected = false;
    try {
        (void)convert_tes_to_mortier(source.string(), source.string());
    } catch (const std::invalid_argument&) {
        alias_rejected = true;
    }
    assert(alias_rejected);
    fs::remove_all(root);
}

int main() {
    test_codec();
    test_stable_hash_encoding();
    test_regular_tilings();
    test_finite_voltage_development();
    test_generated_tes_rejections();
    test_store_and_json();
    test_schema_one_compatibility();
    test_tes_fingerprint();
    test_tes_conversion();
}
