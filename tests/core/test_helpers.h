#pragma once

#include "core/hash.h"
#include "core/util.h"

// A temporary directory removed when the object goes out of scope.
struct TempDir {
    fp::fs::path path;
    TempDir() : path(fp::fs::temp_directory_path() / ("freepen-test-" + fp::randomToken(6)))
    {
        fp::fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fp::fs::remove_all(path, ec);
    }
};
