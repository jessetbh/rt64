//
// RT64
//

// [wcw] Tests for texture pack folders (common/rt64_filesystem_directory.h) against real files in the temp folder:
// a pack's files are found, sized, loaded and canonicalized by the '/' paths rt64.json, Rice names and the database
// use, from subfolders, with "." and "..", and past MAX_PATH (aki-roadmap#115 and its review).

#include "common/rt64_filesystem_directory.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "wcw_test.h"

using namespace RT64;

namespace {
    // A pack folder in the temp folder, removed again at the end of the test.
    struct TempPack {
        std::filesystem::path root;

        explicit TempPack(const std::string &name) {
            root = std::filesystem::temp_directory_path() / ("rt64_wcw_packfs_" + name + "_" + std::to_string(std::random_device{}()));
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
            std::filesystem::create_directories(root, ec);
        }

        ~TempPack() {
            std::error_code ec;
            std::filesystem::remove_all(wcwLongPathRoot(root), ec);
        }

        // Writes a file under the pack; relative uses '\' or '/' (written through the \\?\ form, so any length works).
        void write(const std::string &relative, const std::string &content) const {
            const std::filesystem::path path = wcwPackPath(wcwLongPathRoot(root), relative);
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream file(path, std::ios::binary);
            file << content;
        }
    };

    bool loads(const FileSystem &fileSystem, const std::string &path, const std::string &expected) {
        std::string data(expected.size(), '\0');
        return fileSystem.load(path, reinterpret_cast<uint8_t *>(data.data()), data.size()) && (data == expected);
    }
}

TEST_CASE("pack folders: a texture in a subfolder, named with '/', is found, sized, loaded and canonicalized (#115 review)") {
    // With the \\?\ root, '/' in a pack path wasn't converted by Win32: every subfolder texture silently stayed stock.
    TempPack pack("sub");
    pack.write("textures/sub/x.png", "hello");
    std::unique_ptr<FileSystem> fileSystem = FileSystemDirectory::create(pack.root);
    CHECK(fileSystem != nullptr);
    CHECK(fileSystem->exists("textures/sub/x.png"));
    CHECK_EQ(fileSystem->getSize("textures/sub/x.png"), size_t(5));
    CHECK(loads(*fileSystem, "textures/sub/x.png", "hello"));
    CHECK(FileSystem::toForwardSlashes(fileSystem->makeCanonical("textures/sub/x.png")) == "textures/sub/x.png");
    CHECK(!fileSystem->exists("textures/sub/missing.png"));
}

TEST_CASE("pack folders: '.' and '..' in a pack path resolve against the pack root") {
    TempPack pack("dots");
    pack.write("textures/sub/x.png", "hello");
    std::unique_ptr<FileSystem> fileSystem = FileSystemDirectory::create(pack.root);
    CHECK(fileSystem != nullptr);
    CHECK(fileSystem->exists("textures/other/../sub/x.png"));
    CHECK(fileSystem->exists("./textures/sub/./x.png"));
    CHECK(loads(*fileSystem, "textures/other/../sub/x.png", "hello"));
    CHECK(FileSystem::toForwardSlashes(fileSystem->makeCanonical("textures/other/../sub/x.png")) == "textures/sub/x.png");
}

TEST_CASE("pack folders: a texture past MAX_PATH is found and loaded, and the walk lists it") {
    TempPack pack("long");
    std::string relative = "textures";
    while ((pack.root.u8string().size() + relative.size()) < 300) {
        relative += "/a_rather_long_folder_name";
    }

    relative += "/x.png";
    pack.write(relative, "hello");
    std::unique_ptr<FileSystem> fileSystem = FileSystemDirectory::create(pack.root);
    CHECK(fileSystem != nullptr);
    CHECK(fileSystem->exists(relative));
    CHECK(loads(*fileSystem, relative, "hello"));
    size_t files = 0;
    std::string listed;
    for (const std::string &path : *fileSystem) {
        files++;
        listed = FileSystem::toForwardSlashes(path);
    }

    CHECK_EQ(files, size_t(1));
    CHECK(listed == relative);
}
