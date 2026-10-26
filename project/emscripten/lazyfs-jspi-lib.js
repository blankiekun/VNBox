// JSPI builds (-sJSPI --js-library=lazyfs-jspi-lib.js): fd_read/fd_pread become suspending
// imports. Reads of lazy game files (lazyfs.js, jspi mode) return a promise and suspend the wasm
// stack until the File read settles; every other read stays synchronous. Only reads: V8 throws
// SuspendError on any call of a suspending import outside boxedwine_tick (event handlers log
// through fd_write), even one that returns no promise.
addToLibrary({
  fd_read__postset: 'globalThis.boxedwineJspiReads = true;', // lazyfs.js picks jspi mode when this global exists
  fd_read__deps: ['$doReadv'],
  fd_read__async: 'auto',
  fd_read: (fd, iov, iovcnt, pnum) => {
    var stream = SYSCALLS.getStreamFromFD(fd);
    if (stream.node.lazyAsync) {
      return lazyReadvAsync(stream, iov, iovcnt).then((num) => { {{{ makeSetValue('pnum', 0, 'num', SIZE_TYPE) }}}; return 0; },
        (e) => { vnboxReport('lazyfs', 'read of ' + stream.path + ' failed: ' + e); return {{{ cDefs.EIO }}}; });
    }
    var num = doReadv(stream, iov, iovcnt);
    {{{ makeSetValue('pnum', 0, 'num', SIZE_TYPE) }}};
    return 0;
  },
  fd_pread__deps: ['$doReadv'],
  fd_pread__i53abi: true,
  fd_pread__async: 'auto',
  fd_pread: (fd, iov, iovcnt, offset, pnum) => {
    if (isNaN(offset)) return {{{ cDefs.EFBIG }}};
    var stream = SYSCALLS.getStreamFromFD(fd);
    if (stream.node.lazyAsync) {
      return lazyReadvAsync(stream, iov, iovcnt, offset).then((num) => { {{{ makeSetValue('pnum', 0, 'num', SIZE_TYPE) }}}; return 0; },
        (e) => { vnboxReport('lazyfs', 'read of ' + stream.path + ' failed: ' + e); return {{{ cDefs.EIO }}}; });
    }
    var num = doReadv(stream, iov, iovcnt, offset);
    {{{ makeSetValue('pnum', 0, 'num', SIZE_TYPE) }}};
    return 0;
  },
});
