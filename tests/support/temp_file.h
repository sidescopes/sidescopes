#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace sidescopes::test {

// A scoped temporary directory reserved exclusively for one fixture, including
// when another test process uses the same name. Only its own contents are
// removed on destruction.
class TempDir
{
public:
    explicit TempDir(std::string_view name)
    {
        const auto base = std::filesystem::temp_directory_path() / "sidescopes-tests";
        std::filesystem::create_directories(base);
        static std::atomic<unsigned long long> sequence{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 64; ++attempt) {
            m_path =
                base / (std::string{name} + '-' + std::to_string(stamp) + '-' + std::to_string(sequence.fetch_add(1)));
            if (std::filesystem::create_directory(m_path)) {
                return;
            }
        }
        throw std::runtime_error("Could not reserve a temporary test directory");
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

// A scoped temporary file path in its own directory. The file is created by
// whatever writes to it; the directory removes it even after an assertion.
class TempFile
{
public:
    explicit TempFile(std::string_view name)
        : m_directory(name),
          m_path(m_directory.file(name))
    {
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const
    {
        return m_path;
    }

    // Writes content to the file, creating or truncating it.
    void write(std::string_view content) const
    {
        std::ofstream output(m_path);
        output << content;
        output.close();
        if (!output) {
            throw std::runtime_error("Could not write a temporary test file");
        }
    }

private:
    TempDir m_directory;
    std::filesystem::path m_path;
};

}  // namespace sidescopes::test
