/*
 *  /dev/vnbox-movie: lets VNBox's quartz (vn/wine/quartz-browser-movies.patch) hand a movie to the
 *  browser page, which decodes and draws it over the canvas (vnbox-movie.js) instead of the guest.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "boxedwine.h"

#include "../../io/fsvirtualopennode.h"
#include "devmovie.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS_DEPS(boxedwine_movie_deps, "$UTF8ToString");
#endif

// One open = one movie. Each write is one text command:
//   open <guest path>   the file to play (the page reads it from the Emscripten FS itself)
//   rect <x> <y> <w> <h> the video window in screen pixels; w 0 = hidden
//   run | pause | stop | seek <ms> | volume <0-1000>
// A read returns "1\n" once the movie has ended (or failed after open), "0\n" while it plays,
// "-1\n" when the page didn't take the file (no such file, movies=0, no page): quartz then
// builds the graph the normal way.
class DevMovie : public FsVirtualOpenNode {
public:
    DevMovie(const std::shared_ptr<FsNode>& node, U32 flags) : FsVirtualOpenNode(node, flags), id(++lastId) {}

    U32 readNative(U8* buffer, U32 len) override {
        const char* s = !accepted ? "-1\n" : (call("status", "") ? "1\n" : "0\n");
        U32 n = std::min(len, (U32)strlen(s));
        memcpy(buffer, s, n);
        return n;
    }

    U32 writeNative(U8* buffer, U32 len) override {
        std::string line((const char*)buffer, len);
        while (!line.empty() && (line.back() == '\n' || line.back() == 0)) line.pop_back();
        size_t space = line.find(' ');
        std::string cmd = line.substr(0, space), arg = space == std::string::npos ? "" : line.substr(space + 1);
        if (cmd == "open") {
            std::shared_ptr<FsNode> file = Fs::getNodeFromLocalPath(B(""), BString::copy(arg.c_str()), true);
            accepted = file && !file->isDirectory() && call("open", file->getNativePathForData().c_str()) == 1;
        } else if (accepted) {
            call(cmd.c_str(), arg.c_str());
        }
        return len;
    }

    void close() override {
        if (accepted) call("close", "");
        accepted = false;
    }

private:
    int call(const char* cmd, const char* arg) {
#ifdef __EMSCRIPTEN__
        return MAIN_THREAD_EM_ASM_INT({
            return typeof boxedwineMovie === "function" ? boxedwineMovie($0, UTF8ToString($1), UTF8ToString($2)) : -1;
        }, id, cmd, arg);
#else
        return -1;
#endif
    }

    static inline U32 lastId = 0;
    U32 id;
    bool accepted = false;
};

FsOpenNode* openDevMovie(const std::shared_ptr<FsNode>& node, U32 flags, U32 data) {
    return new DevMovie(node, flags);
}
