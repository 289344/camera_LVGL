// 无摄像头测试：相册相对路径 -> 播放 URI，兼容板端 C++14。
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include "file_manager/file_manager.h"
#include "fs_compat.h"

int main() {
    char *cwd = ::getcwd(nullptr, 0);
    assert(cwd);
    char temp[] = "/tmp/camera_playback_uri_XXXXXX";
    assert(::mkdtemp(temp));
    assert(::chdir(temp) == 0);
    assert(fscompat::createDirectories("recordings"));

    const std::string names[] = {"rec_20261022_145227.mp4", "a #?%\"'.mp4", "录像.mp4"};
    const std::string encoded[] = {"rec_20261022_145227.mp4", "a%20%23%3F%25%22%27.mp4",
                                   "%E5%BD%95%E5%83%8F.mp4"};
    const std::string prefix = "file://" + std::string(temp) + "/recordings/";
    for (size_t i = 0; i < 3; ++i) {
        const std::string path = "recordings/" + names[i];
        std::FILE *file = std::fopen(path.c_str(), "wb");
        assert(file);
        assert(std::fclose(file) == 0);
        assert(fscompat::fileUri(path) == prefix + encoded[i]);
        assert(fscompat::fileUri(std::string(temp) + "/" + path) == prefix + encoded[i]);
    }

    // 使用相册真正返回的路径，覆盖导致本次故障的 recordings/... 输入。
    const auto files = filemanager::FileManager::list("recordings");
    assert(files.size() == 3);
    for (const auto &file : files) {
        assert(file.path == "recordings/" + file.name);
        bool matched = false;
        for (size_t i = 0; i < 3; ++i) {
            if (file.name == names[i]) {
                assert(fscompat::fileUri(file.path) == prefix + encoded[i]);
                matched = true;
            }
        }
        assert(matched);
    }
    assert(fscompat::fileUri("recordings/../recordings/" + names[0]) == prefix + encoded[0]);
    assert(::symlink(names[0].c_str(), "recordings/link.mp4") == 0);
    assert(fscompat::fileUri("recordings/link.mp4") == prefix + encoded[0]);
    assert(fscompat::fileUri("recordings/missing.mp4").empty());
    assert(errno == ENOENT);

    assert(::unlink("recordings/link.mp4") == 0);
    for (const auto &name : names) assert(::unlink(("recordings/" + name).c_str()) == 0);
    assert(::rmdir("recordings") == 0);
    assert(::chdir(cwd) == 0);
    std::free(cwd);
    assert(::rmdir(temp) == 0);
    std::puts("PASS: album playback URI (relative/absolute, UTF-8, reserved characters, missing file)");
}
