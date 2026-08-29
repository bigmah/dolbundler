/**
 * A race-free replacement for emscripten's WASMFS fetch backend.
 *
 * The stock one (src/lib/libwasmfs_fetch.js) initialises a file's range table
 * like this:
 *
 *     if (!(file in wasmFS$JSMemoryRanges)) {
 *       var fileInfo = await fetch(url, {method:'HEAD', ...});   // <-- await
 *       wasmFS$JSMemoryRanges[file] = { size, chunks: [], chunkSize };
 *     }
 *
 * Two reads of the same file that arrive before either HEAD resolves both pass
 * the `in` test, and the second one **replaces the object** -- discarding the
 * chunks the first had already stored. The first read then continues past its
 * own await, looks the table up again, finds `chunks[i]` undefined, and throws
 * a TypeError out of the async function. Dolphin reads the disc from more than
 * one thread, so this is reachable, and what it produces downstream is a read
 * that moved no bytes into a freshly zeroed buffer: a texture whose source is
 * all zeros and a surface that renders black, with no error anywhere.
 *
 * The fixes here are all about doing each piece of work exactly once:
 *
 *   - one in-flight promise per file for the HEAD, shared by every concurrent
 *     first read, so the table is created once and never replaced;
 *   - one in-flight promise per (file, chunk) for the range fetch, so two reads
 *     over the same chunk cannot interleave a half-filled write;
 *   - a missing chunk at read time returns EIO instead of throwing, because a
 *     rejected read here is indistinguishable from a short one and silence is
 *     how this cost a day.
 */
addToLibrary({
  $wasmFS$JSMemoryRanges: {},
  // Per-file and per-chunk work in flight, so it is never started twice.
  $wasmFS$fetchPending: {},

  _wasmfs_create_fetch_backend_js__deps: [
    '$wasmFS$backends',
    '$wasmFS$JSMemoryRanges',
    '$wasmFS$fetchPending',
    '_wasmfs_fetch_get_file_url',
    '_wasmfs_fetch_get_chunk_size',
  ],
  _wasmfs_create_fetch_backend_js: async function(backend) {
    function urlFor(file) {
      var fileUrl = UTF8ToString(__wasmfs_fetch_get_file_url(file));
      if (fileUrl.indexOf('://') !== -1) return fileUrl;
      try {
        return new URL(fileUrl, self.location.origin).toString();
      } catch (e) {
        throw {status: 404};
      }
    }

    // The HEAD, once per file however many readers are waiting on it.
    function ensureFile(file, chunkSize) {
      if (file in wasmFS$JSMemoryRanges) return Promise.resolve();
      var key = 'file:' + file;
      if (key in wasmFS$fetchPending) return wasmFS$fetchPending[key];
      var url = urlFor(file);
      var pending = (async () => {
        var head = await fetch(url, {method: 'HEAD', headers: {'Range': 'bytes=0-'}});
        var length = head.ok && head.headers.has('Content-Length')
                   ? parseInt(head.headers.get('Content-Length'), 10) : NaN;
        // Repeated headers arrive comma-joined, so test the *first* value
        // rather than the whole string: "bytes, bytes" is still a server that
        // supports ranges, and reading it as "no" is what pulled a 1.2 GB disc
        // into memory a file at a time.
        var acceptRanges = (head.headers.get('Accept-Ranges') || '')
                             .split(',')[0].trim();
        if (head.ok && acceptRanges === 'bytes' && length > chunkSize * 2) {
          wasmFS$JSMemoryRanges[file] = {size: length, chunks: [], chunkSize: chunkSize};
        } else {
          var whole = await fetch(url);
          if (!whole.ok) throw whole;
          var bytes = new Uint8Array(await whole.arrayBuffer());
          wasmFS$JSMemoryRanges[file] =
            {size: bytes.byteLength, chunks: [bytes], chunkSize: bytes.byteLength || 1};
        }
      })();
      wasmFS$fetchPending[key] = pending;
      // Kept until it settles, then dropped so a failure can be retried.
      pending.then(() => { delete wasmFS$fetchPending[key]; },
                   () => { delete wasmFS$fetchPending[key]; });
      return pending;
    }

    // One span of chunks, once. Readers that overlap wait on the same promise
    // rather than racing to fill the same array slots.
    function ensureChunks(file, firstChunk, lastChunk) {
      var info = wasmFS$JSMemoryRanges[file];
      var chunkSize = info.chunkSize;
      var waits = [];
      for (var i = firstChunk; i <= lastChunk; i++) {
        if (info.chunks[i]) continue;
        var key = 'chunk:' + file + ':' + i;
        if (key in wasmFS$fetchPending) { waits.push(wasmFS$fetchPending[key]); continue; }
        waits.push(fetchChunk(file, i, key));
      }
      return waits.length ? Promise.all(waits) : Promise.resolve();
    }

    function fetchChunk(file, index, key) {
      var info = wasmFS$JSMemoryRanges[file];
      var chunkSize = info.chunkSize;
      var url = urlFor(file);
      var start = index * chunkSize;
      var end = Math.min(start + chunkSize, info.size);
      var pending = (async () => {
        var response = await fetch(url, {headers: {'Range': `bytes=${start}-${end - 1}`}});
        if (!response.ok) throw response;
        var bytes = new Uint8Array(await response.arrayBuffer());
        // Only ever fills a slot that is still empty, and only its own.
        if (!wasmFS$JSMemoryRanges[file].chunks[index])
          wasmFS$JSMemoryRanges[file].chunks[index] = bytes;
      })();
      wasmFS$fetchPending[key] = pending;
      pending.then(() => { delete wasmFS$fetchPending[key]; },
                   () => { delete wasmFS$fetchPending[key]; });
      return pending;
    }

    wasmFS$backends[backend] = {
      allocFile: async (file) => {},
      freeFile: async (file) => { delete wasmFS$JSMemoryRanges[file]; },
      write: async (file, buffer, length, offset) => {
        console.error('TODO: file writing in fetch backend? read-only for now');
      },

      read: async (file, buffer, length, offset) => {
        if (offset < 0 || length <= 0) return 0;
        var chunkSize = __wasmfs_fetch_get_chunk_size(file);
        try {
          await ensureFile(file, chunkSize);
          var info = wasmFS$JSMemoryRanges[file];
          var want = Math.min(length, info.size - offset);
          if (want <= 0) return 0;
          await ensureChunks(file, (offset / info.chunkSize) | 0,
                             ((offset + want - 1) / info.chunkSize) | 0);
        } catch (failed) {
          return failed && failed.status === 404 ? -{{{ cDefs.ENOENT }}}
                                                 : -{{{ cDefs.EBADF }}};
        }
        var info = wasmFS$JSMemoryRanges[file];
        length = Math.min(length, info.size - offset);
        if (length <= 0) return 0;
        var chunks = info.chunks;
        var size = info.chunkSize;
        var firstChunk = (offset / size) | 0;
        var lastChunk = ((offset + length - 1) / size) | 0;
        var readLength = 0;
        for (var i = firstChunk; i <= lastChunk; i++) {
          var chunk = chunks[i];
          // Loudly, not by throwing: a rejected read reaches C++ as a short one
          // and a short one silently leaves the caller's buffer as it was.
          if (!chunk) {
            console.error(`wasmfs fetch: chunk ${i} of file ${file} missing after fetch`);
            return -{{{ cDefs.EIO }}};
          }
          var chunkStart = i * size;
          var start = Math.max(chunkStart, offset);
          var end = Math.min(chunkStart + size, offset + length);
          HEAPU8.set(chunk.subarray(start - chunkStart, end - chunkStart),
                     buffer + (start - offset));
          readLength = end - offset;
        }
        return readLength;
      },

      getSize: async (file) => {
        try {
          await ensureFile(file, __wasmfs_fetch_get_chunk_size(file));
        } catch (e) {
          return 0;
        }
        return wasmFS$JSMemoryRanges[file].size;
      },
    };
  },
});
