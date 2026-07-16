#pragma once

#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

namespace sidescopes::test {

// A scoped temporary file path under the system temp directory that removes
// itself when it goes out of scope, so a thrown Catch2 assertion never strands
// a file behind - the manual remove the preferences tests used leaked exactly
// there. The parent directory is created on construction; the file itself is
// created by whatever writes to it.
class TempFile
{
public:
    explicit TempFile(std::string_view name)
        : m_path(std::filesystem::temp_directory_path() / "sidescopes-tests" / name)
    {
        std::error_code error;
        std::filesystem::create_directories(m_path.parent_path(), error);
        std::filesystem::remove(m_path, error);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;

    ~TempFile()
    {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const
    {
        return m_path;
    }

    // Writes content to the file, creating or truncating it.
    void write(std::string_view content) const
    {
        std::ofstream(m_path) << content;
    }

private:
    std::filesystem::path m_path;
};

// A scoped temporary directory: created empty on construction, removed with
// everything under it on destruction.
class TempDir
{
public:
    explicit TempDir(std::string_view name)
        : m_path(std::filesystem::temp_directory_path() / "sidescopes-tests" / name)
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
        std::filesystem::create_directories(m_path, error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    ~TempDir()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const
    {
        return m_path;
    }

    // A path to a child entry, which the caller creates.
    [[nodiscard]] std::filesystem::path file(std::string_view name) const
    {
        return m_path / name;
    }

private:
    std::filesystem::path m_path;
};

}  // namespace sidescopes::test
