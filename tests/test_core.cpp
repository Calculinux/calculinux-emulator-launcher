// Unit tests for the launcher core. No network: downloads use file:// URLs.

#include "core/catalog.h"
#include "core/download.h"
#include "core/index.h"
#include "core/session.h"
#include "core/sha256.h"
#include "core/state.h"
#include "core/util.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

using namespace cl;

static int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            failures++;                                                \
        }                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                 \
    do {                                                               \
        auto va = (a);                                                 \
        auto vb = (b);                                                 \
        if (!(va == vb)) {                                             \
            fprintf(stderr, "%s:%d: CHECK_EQ(%s, %s) failed\n", __FILE__, __LINE__, #a, #b); \
            failures++;                                                \
        }                                                              \
    } while (0)

static fs::path make_temp_dir() {
    std::string tmpl = (fs::temp_directory_path() / "launcher-test-XXXXXX").string();
    char* dir = mkdtemp(tmpl.data());
    if (!dir) {
        perror("mkdtemp");
        exit(1);
    }
    return dir;
}

static void write(const fs::path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << data;
}

static std::string slurp(const fs::path& path) {
    std::string data;
    read_file(path, data);
    return data;
}

static void test_sha256() {
    // FIPS 180-4 examples.
    CHECK_EQ(sha256_hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(sha256_hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    // Padding boundaries: 55, 56, 63, 64, 65 bytes, and a million 'a'.
    CHECK_EQ(sha256_hex(std::string(55, 'a')),
             std::string("9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"));
    CHECK_EQ(sha256_hex(std::string(56, 'a')),
             std::string("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"));
    CHECK_EQ(sha256_hex(std::string(64, 'a')),
             std::string("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"));
    CHECK_EQ(sha256_hex(std::string(1000000, 'a')),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

    fs::path dir = make_temp_dir();
    write(dir / "f", std::string(3000000, 'x'));
    std::string hex;
    std::atomic<uint64_t> done{0};
    CHECK(sha256_file(dir / "f", hex, &done));
    CHECK_EQ(hex, sha256_hex(std::string(3000000, 'x')));
    CHECK_EQ(done.load(), (uint64_t)3000000);
    CHECK(!sha256_file(dir / "missing", hex));
    fs::remove_all(dir);
}

static void test_util() {
    CHECK(valid_id("v1.0.0-alpha8"));
    CHECK(valid_id("continuous-main"));
    CHECK(valid_id("1.0.0+abc"));
    CHECK(!valid_id(""));
    CHECK(!valid_id(".."));
    CHECK(!valid_id(".hidden"));
    CHECK(!valid_id("a/b"));
    CHECK(!valid_id("a b"));
    CHECK(!valid_id(std::string(65, 'a')));
    CHECK(reserved_id("legacy") && reserved_id("local") && reserved_id("custom"));
    CHECK_EQ(first_token("abc123  file.qcow2\n"), std::string("abc123"));
    CHECK_EQ(first_token("  \n"), std::string(""));
    CHECK_EQ(human_size(463863808), std::string("442 MB"));
}

static const char* kIndex = R"JSON({
  "schema": 1,
  "machine": "calculinux-qemuarm",
  "latest_release": "v1.0.0",
  "images": [
    {"id": "continuous-main", "channel": "continuous-main", "label": "Latest (main)",
     "version": "1.0.0-continuous+abc", "size": 100, "sha256": "aa", "url": "https://x/main.qcow2"},
    {"id": "v1.0.0", "channel": "release", "version": "v1.0.0", "size": 200, "sha256": "bb",
     "url": "https://x/v1.0.0.qcow2", "future_field": [1, 2]},
    {"id": "../evil", "channel": "release", "url": "https://x/evil"},
    {"id": "legacy", "channel": "release", "url": "https://x/reserved"},
    {"id": "no-url", "channel": "release"},
    {"id": "v1.0.0", "channel": "release", "url": "https://x/duplicate"},
    "not an object"
  ]
})JSON";

static void test_index() {
    Index index;
    std::string error;
    CHECK(parse_index(kIndex, index, error));
    CHECK_EQ(index.images.size(), (size_t)2);
    CHECK_EQ(index.images[0].id, std::string("continuous-main"));
    CHECK_EQ(index.images[0].label, std::string("Latest (main)"));
    CHECK_EQ(index.images[0].size, (uint64_t)100);
    CHECK_EQ(index.images[1].label, std::string("v1.0.0"));  // defaults to the id
    CHECK_EQ(index.images[1].url, std::string("https://x/v1.0.0.qcow2"));
    CHECK_EQ(index.latest_release, std::string("v1.0.0"));

    CHECK(!parse_index("not json", index, error));
    CHECK(!parse_index("[]", index, error));
    CHECK(!parse_index("{\"images\": 3}", index, error));
    CHECK(parse_index("{\"images\": [], \"latest_release\": \"gone\"}", index, error));
    CHECK(index.images.empty());
    CHECK(index.latest_release.empty());  // not in the list
}

// A layout 1 directory as AppRun left it, with the downloaded image in use.
static void test_migrate_downloaded() {
    fs::path dir = make_temp_dir();
    write(dir / "images/calculinux.qcow2", "base");
    write(dir / "images/calculinux.qcow2.sha256", "abc123  calculinux-image.qcow2\n");
    write(dir / "images/calculinux.qcow2.part", "junk");
    write(dir / "disk.qcow2", "overlay");
    write(dir / "uboot-env.img", "env");
    write(dir / "image-id", "abc123\n");

    State state(dir);
    std::string error;
    CHECK(state.prepare(error));
    CHECK_EQ(slurp(dir / "layout"), std::string("2\n"));
    CHECK_EQ(slurp(state.base_path("legacy")), std::string("base"));
    CHECK_EQ(state.installed_sha("legacy"), std::string("abc123"));
    CHECK_EQ(slurp(state.image_dir("legacy") / "disk.qcow2"), std::string("overlay"));
    CHECK_EQ(slurp(state.image_dir("legacy") / "uboot-env.img"), std::string("env"));
    CHECK_EQ(slurp(state.image_dir("legacy") / "image-id"), std::string("abc123\n"));
    CHECK(!fs::exists(dir / "disk.qcow2"));
    CHECK(!fs::exists(dir / "images/calculinux.qcow2"));
    CHECK(!fs::exists(dir / "images/calculinux.qcow2.sha256"));
    CHECK(!fs::exists(dir / "images/calculinux.qcow2.part"));
    CHECK_EQ(state.load_settings().last_id, std::string("legacy"));
    CHECK(state.has_state("legacy"));

    // Running again changes nothing.
    CHECK(state.prepare(error));
    CHECK_EQ(slurp(state.base_path("legacy")), std::string("base"));

    // The index shows which published image it is.
    Index index;
    CHECK(parse_index(R"JSON({"images": [
        {"id": "other", "url": "u", "sha256": "zzz"},
        {"id": "continuous-main", "url": "u", "sha256": "abc123", "label": "Latest (main)", "version": "1.0"}]})JSON",
                      index, error));
    CHECK_EQ(state.adopt_legacy(index), std::string("continuous-main"));
    CHECK(!fs::exists(state.image_dir("legacy")));
    CHECK_EQ(slurp(state.image_dir("continuous-main") / "disk.qcow2"), std::string("overlay"));
    CHECK_EQ(state.load_settings().last_id, std::string("continuous-main"));
    ImageMeta meta;
    CHECK(state.load_meta("continuous-main", meta));
    CHECK_EQ(meta.label, std::string("Latest (main)"));
    CHECK_EQ(state.adopt_legacy(index), std::string(""));
    fs::remove_all(dir);
}

// A layout 1 directory last used with --image FILE: the overlay is that
// file's, not the downloaded image's.
static void test_migrate_local() {
    fs::path dir = make_temp_dir();
    write(dir / "images/calculinux.qcow2", "base");
    write(dir / "disk.qcow2", "overlay");
    write(dir / "uboot-env.img", "env");
    write(dir / "image-id", "local 123 456 /some/file.qcow2\n");

    State state(dir);
    std::string error;
    CHECK(state.prepare(error));
    CHECK_EQ(slurp(dir / "local/disk.qcow2"), std::string("overlay"));
    CHECK_EQ(slurp(dir / "local/image-id"), std::string("local 123 456 /some/file.qcow2\n"));
    CHECK(state.is_downloaded("legacy"));
    CHECK(!state.has_state("legacy"));
    // No .sha256 came with the image: it is computed.
    CHECK_EQ(state.installed_sha("legacy"), sha256_hex("base"));
    fs::remove_all(dir);
}

static void test_fresh_state() {
    fs::path dir = make_temp_dir() / "nested/state";
    State state(dir);
    std::string error;
    CHECK(state.prepare(error));
    CHECK(fs::exists(dir / "layout"));
    CHECK(state.downloaded_ids().empty());
    CHECK(state.load_settings().last_id.empty());

    Settings settings;
    settings.last_id = "v1.0.0";
    settings.ssh_port = "2223";
    CHECK(state.save_settings(settings));
    CHECK_EQ(state.load_settings().last_id, std::string("v1.0.0"));
    CHECK_EQ(state.load_settings().ssh_port, std::string("2223"));
    write(dir / "settings", "last_id=../../etc\nssh_port=off\n");
    CHECK(state.load_settings().last_id.empty());
    CHECK_EQ(state.load_settings().ssh_port, std::string("off"));
    fs::remove_all(dir.parent_path().parent_path());
}

static IndexEntry entry_for(const fs::path& file, const std::string& id) {
    IndexEntry e;
    e.id = id;
    e.channel = "release";
    e.label = id;
    e.version = "1.0";
    e.url = "file://" + file.string();
    e.sha256 = sha256_hex(slurp(file));
    e.size = fs::file_size(file);
    return e;
}

static void publish(const fs::path& file, const std::string& data) {
    write(file, data);
    write(fs::path(file.string() + ".sha256"), sha256_hex(data) + "  " + file.filename().string() + "\n");
}

static void test_fetch() {
    fs::path dir = make_temp_dir();
    fs::path server = dir / "server";
    State state(dir / "state");
    std::string error;
    CHECK(state.prepare(error));
    HttpOptions http;

    std::string v1(200000, 'a');
    publish(server / "img.qcow2", v1);
    IndexEntry e = entry_for(server / "img.qcow2", "v1.0.0");

    {
        FetchProgress progress;
        FetchResult r = fetch_image(state, e, progress, http);
        CHECK(r.ok());
        CHECK(!r.replaced);
        CHECK_EQ(slurp(state.base_path("v1.0.0")), v1);
        CHECK_EQ(state.installed_sha("v1.0.0"), e.sha256);
        CHECK(!fs::exists(state.part_path("v1.0.0")));
        CHECK(!fs::exists(state.part_meta_path("v1.0.0")));
        ImageMeta meta;
        CHECK(state.load_meta("v1.0.0", meta));
        CHECK_EQ(meta.version, std::string("1.0"));
        CHECK_EQ(meta.size, (uint64_t)200000);
    }

    // Already current: nothing is downloaded (the source could be gone).
    {
        fs::rename(server / "img.qcow2", server / "moved");
        FetchProgress progress;
        CHECK(fetch_image(state, e, progress, http).ok());
        fs::rename(server / "moved", server / "img.qcow2");
    }

    // A newer image under the same id replaces the base and drops the overlay.
    std::string v2(250000, 'b');
    publish(server / "img.qcow2", v2);
    write(state.image_dir("v1.0.0") / "disk.qcow2", "overlay");
    write(state.image_dir("v1.0.0") / "uboot-env.img", "env");
    {
        // The index entry is stale (old sha); <url>.sha256 is what counts.
        FetchProgress progress;
        FetchResult r = fetch_image(state, e, progress, http);
        CHECK(r.ok());
        CHECK(r.replaced);
        CHECK_EQ(slurp(state.base_path("v1.0.0")), v2);
        CHECK_EQ(state.installed_sha("v1.0.0"), sha256_hex(v2));
        CHECK(!state.has_state("v1.0.0"));
    }

    // Resume: a partial download of the right image is completed, not restarted.
    std::string v3 = std::string(100000, 'c') + std::string(100000, 'd');
    publish(server / "resume.qcow2", v3);
    IndexEntry r3 = entry_for(server / "resume.qcow2", "resume");
    write(state.part_path("resume"), std::string(100000, 'c'));
    write(state.part_meta_path("resume"), r3.sha256 + "\n");
    {
        FetchProgress progress;
        CHECK(fetch_image(state, r3, progress, http).ok());
        CHECK_EQ(slurp(state.base_path("resume")), v3);
    }

    // A partial download of some other image is discarded.
    IndexEntry r4 = r3;
    r4.id = "stale";
    write(state.part_path("stale"), std::string(100000, 'z'));
    write(state.part_meta_path("stale"), "0000\n");
    {
        FetchProgress progress;
        CHECK(fetch_image(state, r4, progress, http).ok());
        CHECK_EQ(slurp(state.base_path("stale")), v3);
    }

    // A complete partial file is verified without downloading again.
    IndexEntry r5 = r3;
    r5.id = "complete";
    write(state.part_path("complete"), v3);
    write(state.part_meta_path("complete"), r3.sha256 + "\n");
    {
        FetchProgress progress;
        CHECK(fetch_image(state, r5, progress, http).ok());
        CHECK_EQ(slurp(state.base_path("complete")), v3);
    }

    // Corrupt download: reported, and the partial file is not kept.
    write(server / "bad.qcow2", "corrupt");
    write(server / "bad.qcow2.sha256", std::string(64, '0') + "  bad.qcow2\n");
    IndexEntry bad = entry_for(server / "bad.qcow2", "bad");
    {
        FetchProgress progress;
        FetchResult r = fetch_image(state, bad, progress, http);
        CHECK(r.error == FetchError::Checksum);
        CHECK(!state.is_downloaded("bad"));
        CHECK(!fs::exists(state.part_path("bad")));
    }

    // Missing source.
    IndexEntry gone;
    gone.id = "gone";
    gone.url = "file://" + (server / "gone.qcow2").string();
    {
        FetchProgress progress;
        CHECK(fetch_image(state, gone, progress, http).error == FetchError::Network);
    }

    // Cancelled before it starts.
    publish(server / "cancel.qcow2", v1);
    IndexEntry c = entry_for(server / "cancel.qcow2", "cancel");
    {
        FetchProgress progress;
        progress.transfer.cancel = true;
        CHECK(fetch_image(state, c, progress, http).error == FetchError::Cancelled);
        CHECK(!state.is_downloaded("cancel"));
    }

    // Catalog over this state.
    Index index;
    IndexEntry updated = e;  // v1.0.0 is installed at v2; the index now says v3
    updated.sha256 = r3.sha256;
    index.images = {updated, r3, c};
    index.latest_release = "resume";
    std::vector<CatalogRow> rows = build_catalog(index, state, true);
    CHECK_EQ(rows.size(), (size_t)5);
    CHECK(rows[0].status == ImageStatus::UpdateAvailable);
    CHECK(rows[1].status == ImageStatus::Downloaded);
    CHECK(rows[2].status == ImageStatus::NotDownloaded);
    CHECK_EQ(rows[3].entry.id, std::string("complete"));
    CHECK(rows[3].status == ImageStatus::Unpublished);
    CHECK_EQ(rows[4].entry.id, std::string("stale"));
    CHECK_EQ(rows[4].entry.size, (uint64_t)200000);

    rows = build_catalog(index, state, false);
    CHECK_EQ(rows.size(), (size_t)4);  // "cancel" is not downloaded
    CHECK(rows[0].status == ImageStatus::Downloaded);
    CHECK(rows[2].status == ImageStatus::Downloaded);

    rows = build_catalog(index, state, true);
    CHECK_EQ(pick_default(rows, "resume", "v1.0.0", ""), 1);
    CHECK_EQ(pick_default(rows, "missing", "cancel", ""), 2);
    CHECK_EQ(pick_default(rows, "", "", "resume"), 1);
    CHECK_EQ(pick_default(rows, "", "", ""), 0);
    CHECK_EQ(pick_default({}, "a", "b", "c"), -1);

    // Reset and remove.
    write(state.image_dir("resume") / "disk.qcow2", "overlay");
    CHECK(state.reset_image("resume", error));
    CHECK(!state.has_state("resume"));
    CHECK(state.is_downloaded("resume"));
    Settings settings;
    settings.last_id = "resume";
    state.save_settings(settings);
    CHECK(state.remove_image("resume", error));
    CHECK(!fs::exists(state.image_dir("resume")));
    CHECK(state.load_settings().last_id.empty());
    CHECK(!state.remove_image("resume", error));
    CHECK(!state.remove_image("../state", error));

    fs::remove_all(dir);
}

static void test_session() {
    fs::path dir = make_temp_dir();
    std::string image(1000, 'q');
    publish(dir / "server/img.qcow2", image);
    write(dir / "server/index.json",
          "{\"images\": [{\"id\": \"continuous-main\", \"url\": \"file://" + (dir / "server/img.qcow2").string() +
              "\", \"sha256\": \"" + sha256_hex(image) + "\", \"size\": 1000}]}");

    Config config;
    config.state_dir = dir / "state";
    config.index_url = "file://" + (dir / "server/index.json").string();
    config.fallback_url = "file://" + (dir / "server/img.qcow2").string();
    config.app_version = "test";
    std::string error;

    {
        Session session(config);
        CHECK(session.state.prepare(error));
        session.refresh();
        CHECK(session.online);
        CHECK_EQ(session.rows().size(), (size_t)1);
        CHECK(fs::exists(session.state.cached_index_path()));
    }
    {
        // Index gone: the cached copy is used, and only downloads are listed.
        Config offline = config;
        offline.index_url = "file://" + (dir / "server/missing.json").string();
        Session session(offline);
        session.refresh();
        CHECK(!session.online);
        CHECK(!session.index_error.empty());
        // Nothing downloaded: the fallback image is offered.
        std::vector<CatalogRow> rows = session.rows();
        CHECK_EQ(rows.size(), (size_t)1);
        CHECK_EQ(rows[0].entry.id, std::string(kUnindexedId));

        FetchProgress progress;
        CHECK(fetch_image(session.state, rows[0].entry, progress, session.http).ok());
        rows = session.rows();
        CHECK_EQ(rows.size(), (size_t)1);
        CHECK(rows[0].downloaded());
    }
    {
        // Back online: the fallback download turns out to be continuous-main.
        Session session(config);
        session.refresh();
        std::vector<CatalogRow> rows = session.rows();
        CHECK_EQ(rows.size(), (size_t)1);
        CHECK_EQ(rows[0].entry.id, std::string("continuous-main"));
        CHECK(rows[0].status == ImageStatus::Downloaded);
    }
    fs::remove_all(dir);
}

int main() {
    http_global_init();
    test_sha256();
    test_util();
    test_index();
    test_migrate_downloaded();
    test_migrate_local();
    test_fresh_state();
    test_fetch();
    test_session();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
