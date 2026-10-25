//
// RT64
//

#pragma once

#include "rt64_filesystem.h"
#include "rt64_wcw_logic.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace RT64 {
    // [wcw] aki-roadmap#115: the ports aren't long-path aware, so a pack path past MAX_PATH failed. On Windows the root
    // becomes its extended-length (\\?\) form, which every path built from it inherits; elsewhere it stays as given.
    inline std::filesystem::path wcwLongPathRoot(const std::filesystem::path &directoryPath) {
#ifdef _WIN32
        std::error_code ec;
        const std::filesystem::path absolutePath = std::filesystem::absolute(directoryPath, ec);
        if (ec) {
            return directoryPath;
        }

        return std::filesystem::path(WcwLogic::extendedLengthPath(absolutePath.wstring()));
#else
        return directoryPath;
#endif
    }

    // [wcw] #115: a file inside the pack root. Pack paths use '/' and may hold "." or "..", which Win32 doesn't resolve
    // inside a \\?\ root, so the joined path is normalized there (WcwLogic::extendedLengthJoin).
    inline std::filesystem::path wcwPackPath(const std::filesystem::path &directoryPath, const std::string &path) {
#ifdef _WIN32
        return std::filesystem::path(WcwLogic::extendedLengthJoin(directoryPath.wstring(), std::filesystem::u8path(path).wstring()));
#else
        return directoryPath / std::filesystem::u8path(path);
#endif
    }

    // [wcw] #115: the directory walk uses the error_code overloads. A failed step ends the walk (logged once per walk)
    // instead of throwing filesystem_error out of a render thread, which ended the game with 0xC0000409 and no backtrace.
    inline void wcwLogWalkError(const std::filesystem::path &directoryPath, const std::error_code &ec) {
        fprintf(stderr, "[wcw] texture pack walk stopped in %s: %s\n", directoryPath.u8string().c_str(), ec.message().c_str());
    }

    struct FileSystemDirectoryIterator : FileSystemIteratorImplementation {
        std::filesystem::recursive_directory_iterator dirIterator;
        std::filesystem::path directoryPath;
        std::string pathStr;
        bool pathStrValid = false;
        bool walkErrorLogged = false;

        void logWalkError(const std::error_code &ec) {
            if (!walkErrorLogged) {
                walkErrorLogged = true;
                wcwLogWalkError(directoryPath, ec);
            }
        }

        FileSystemDirectoryIterator() {
            // Default constructor.
        }

        FileSystemDirectoryIterator(const std::filesystem::path &directoryPath) {
            this->directoryPath = directoryPath;
            std::error_code ec;
            dirIterator = std::filesystem::recursive_directory_iterator(directoryPath, ec);
            if (ec) {
                logWalkError(ec);
                dirIterator = std::filesystem::recursive_directory_iterator();
            }
        }

        // Advances one entry; on an error the walk ends.
        void step() {
            std::error_code ec;
            dirIterator.increment(ec);
            if (ec) {
                logWalkError(ec);
                dirIterator = std::filesystem::recursive_directory_iterator();
            }
        }

        bool atFile() const {
            std::error_code ec;
            return dirIterator->is_regular_file(ec) && !ec;
        }

        const std::string &value() override {
            if (!pathStrValid) {
                std::error_code ec;
                pathStr = dirIterator->path().lexically_relative(directoryPath).u8string();
                if (pathStr.empty()) {
                    pathStr = std::filesystem::relative(dirIterator->path(), directoryPath, ec).u8string();
                }
                pathStrValid = true;
            }

            return pathStr;
        }
        
        void increment() override {
            pathStrValid = false;

            static const std::filesystem::recursive_directory_iterator endIterator;
            do {
                step();
            } while ((dirIterator != endIterator) && !atFile());
        }

        bool compare(const FileSystemIteratorImplementation *other) const override {
            const FileSystemDirectoryIterator *otherDirIt = static_cast<const FileSystemDirectoryIterator *>(other);
            return (dirIterator == otherDirIt->dirIterator);
        }
    };

    struct FileSystemDirectory : FileSystem {
        std::filesystem::path directoryPath;
        std::shared_ptr<FileSystemDirectoryIterator> endIterator;

        FileSystemDirectory(const std::filesystem::path &directoryPath) {
            this->directoryPath = wcwLongPathRoot(directoryPath);
            endIterator = { std::make_shared<FileSystemDirectoryIterator>() };
        }
        
        Iterator begin() const override {
            std::shared_ptr<FileSystemDirectoryIterator> beginIterator = std::make_shared<FileSystemDirectoryIterator>(directoryPath);

            // Search for the first iterator that isn't a directory.
            static const std::filesystem::recursive_directory_iterator endIterator;
            while ((beginIterator->dirIterator != endIterator) && !beginIterator->atFile()) {
                beginIterator->step();
            }

            return { beginIterator };
        }

        Iterator end() const override {
            return { endIterator };
        }

        std::string makeCanonical(const std::string &path) const override {
            std::error_code ec;
            std::filesystem::path canonicalPath = std::filesystem::canonical(wcwPackPath(directoryPath, path), ec);
            if (ec) {
                return std::string();
            }

            std::filesystem::path relativePath = std::filesystem::relative(canonicalPath, directoryPath, ec);
            if (ec) {
                return std::string();
            }

            return relativePath.u8string();
        }
        
        bool load(const std::string &path, uint8_t *fileData, size_t fileDataMaxByteCount) const override {
            std::ifstream fileStream(wcwPackPath(directoryPath, path), std::ios::binary);
            if (fileStream.is_open()) {
                fileStream.read((char *)(fileData), fileDataMaxByteCount);
                return !fileStream.bad();
            }
            else {
                return false;
            }
        }

        size_t getSize(const std::string &path) const override {
            std::error_code ec;
            size_t fileSize = std::filesystem::file_size(wcwPackPath(directoryPath, path), ec);
            if (!ec) {
                return fileSize;
            }
            else {
                return 0;
            }
        }

        bool exists(const std::string &path) const override {
            if (path.empty()) {
                return false;
            }

            std::error_code ec;
            return std::filesystem::exists(wcwPackPath(directoryPath, path), ec);
        }

        static std::unique_ptr<FileSystem> create(const std::filesystem::path &directoryPath) {
            std::error_code ec;
            if (std::filesystem::exists(wcwLongPathRoot(directoryPath), ec)) {
                return std::make_unique<FileSystemDirectory>(directoryPath);
            }
            else {
                return nullptr;
            }
        }
    };
};