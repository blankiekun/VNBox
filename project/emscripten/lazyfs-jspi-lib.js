// JSPI builds (-sJSPI --js-library=lazyfs-jspi-lib.js): fd_read/fd_pread become suspending
// imports. Reads of lazy game files (lazyfs.js, jspi mode) return a promise and suspend the wasm
// stack until the File read settles; cache hits and every other read stay synchronous. Only reads: V8 throws
// SuspendError on any call of a suspending import outside boxedwine_tick (event handlers log
// through fd_write), even one that returns no promise.
addToLibrary({
  fd_read__postset: 'globalThis.boxedwineJspiReads = true;', // lazyfs.js picks jspi mode when this global exists
  // A lazy read returns the count when its blocks are cached and only a promise (a suspension) on a miss.
  $lazyJspiRead: (stream, iov, iovcnt, offset, pnum) => {
    var fail = (e) => { vnboxReport('lazyfs', 'read of ' + stream.path + ' failed: ' + e); return {{{ cDefs.EIO }}}; };
    var store = (num) => { {{{ makeSetValue('pnum', 0, 'num', SIZE_TYPE) }}}; return 0; };
    try {
      var num = lazyReadvAsync(stream, iov, iovcnt, offset);
    } catch (e) {
      return fail(e);
    }
    return num instanceof Promise ? num.then(store, fail) : store(num);
  },
  fd_read__deps: ['$doReadv', '$lazyJspiRead'],
  fd_read__async: 'auto',
  fd_read: (fd, iov, iovcnt, pnum) => {
    var stream = SYSCALLS.getStreamFromFD(fd);
    if (stream.node.lazyAsync) return lazyJspiRead(stream, iov, iovcnt, undefined, pnum);
    var num = doReadv(stream, iov, iovcnt);
    {{{ makeSetValue('pnum', 0, 'num', SIZE_TYPE) }}};
    return 0;
  },
  fd_pread__deps: ['$doReadv', '$lazyJspiRead'],
  fd_pread__i53abi: true,
  fd_pread__async: 'auto',
  fd_pread: (fd, iov, iovcnt, offset, pnum) => {
    if (isNaN(offset)) return {{{ cDefs.EFBIG }}};
    var stream = SYSCALLS.getStreamFromFD(fd);
    if (stream.node.lazyAsync) return lazyJspiRead(stream, iov, iovcnt, offset, pnum);
    var num = doReadv(stream, iov, iovcnt, offset);
    {{{ makeSetValue('pnum', 0, 'num', SIZE_TYPE) }}};
    return 0;
  },
});
