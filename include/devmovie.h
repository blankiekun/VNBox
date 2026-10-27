#ifndef __DEVMOVIE_H__
#define __DEVMOVIE_H__

// /dev/vnbox-movie: hands a DirectShow movie to the browser page (VNBox's quartz.dll.so, vn/wine).
FsOpenNode* openDevMovie(const std::shared_ptr<FsNode>& node, U32 flags, U32 data);

#endif
