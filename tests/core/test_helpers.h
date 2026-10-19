#pragma once

#include "core/hash.h"
#include "core/util.h"

// A temporary directory removed when the object goes out of scope.
struct TempDir {
    on::fs::path path;
    TempDir() : path(on::fs::temp_directory_path() / ("ournotes-test-" + on::randomToken(6)))
    {
        on::fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        on::fs::remove_all(path, ec);
    }
};
