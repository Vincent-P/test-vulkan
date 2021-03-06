#pragma once
#include <exo/types.h>
#include <exo/collections/vector.h>

#include <functional>
#include <string>

#if defined(_WIN64)
#    include <array>
#    include <basetsd.h> // win32 types
#    include <wtypes.h>  // HANDLE type
#endif

namespace platform
{
struct Watch
{

#ifdef __linux__

#elif defined(_WIN64)
    HANDLE directory_handle;
    OVERLAPPED overlapped;

    std::array<u8, 2048> buffer;
#endif

    int wd; /* Watch descriptor.  */
    std::string path;
};

struct Event
{

#ifdef __linux__
    u32 mask;   /* Watch mask.  */
    u32 cookie; /* Cookie to synchronize two events.  */
#elif defined(_WIN64)

#endif

    int wd;     /* Watch descriptor.  */
    std::string name; /* filename. */
    usize len;
};

using FileEventF = std::function<void(const Watch &, const Event &)>;

struct FileWatcher
{
#if defined(__linux__)
    int inotify_fd;
#elif defined(_WIN64)

#endif

    Vec<Watch> watches;
    Vec<Event> current_events;
    Vec<FileEventF> callbacks;

    static FileWatcher create();

    Watch add_watch(const char *path);
    void on_file_change(const FileEventF &f);

    void update();
    void destroy();
};
}
