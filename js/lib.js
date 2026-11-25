/*
 * JS emulator library
 * 
 * Copyright (c) 2017 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/* Everything the emulator tells the page goes through optional callbacks on
   Module (set by nerves-vm.js before the emulator script loads):

   onConsoleOutput(str)    console output (byte values as chars)
   getConsoleSize()        [cols, rows] of the console
   onFetch(ev)             {id, url, phase: "start"|"progress"|"done"|"error",
                            loaded, total, status} for every HTTP request
   onDiskInfo(info)        {url, blockBytes, blocks, zeroBlocks,
                            prefetchBlocks} once blk.txt is parsed
   onDiskPersist(info)     {status, saved_bytes, restored_bytes}
   onDownloading(flag)     true while any request is outstanding
   onNetPacket(bytes)      an Ethernet frame sent by the guest
   onReset()               the guest rebooted the machine (riscv_machine.c)
   onPowerOff(status)      the guest powered the machine off
   onSnapshot(info)        restoring a snapshot instead of booting (when
                           blk.txt names one and no disk writes are saved):
                           {url, phase: "start"|"progress"|"restored"|
                            "skipped"|"error", loaded, total, extra, message}
   snapshot                false: never restore a snapshot
   waitForDisk             false: run the machine while the disk image is
                           still downloading (blocks it needs first are
                           fetched right away); by default it starts once
                           the whole image is local (and the snapshot, if
                           any, is restored)
   onWaiting(info)         {disk, snapshot}: what the machine waits for
                           before it runs
   onRun(info)             {restored}: the machine starts running
   onSnapshotSaved(bytes)  the result of ccall("vm_snapshot_save"): the
                           snapshot file, or null if the machine is busy
                           (try again); snapshotExtra (a Uint8Array) is
                           stored in it and given back as info.extra */
mergeInto(LibraryManager.library, {
    $WgetState: {
        nextHandle: 1,
        requests: {}
    },

    console_write: function(opaque, buf, len)
    {
        var str;
        /* Note: we really send byte values. It would be up to the
         * terminal to support UTF-8 */
        str = String.fromCharCode.apply(String, HEAPU8.subarray(buf, buf + len));
        if (Module.onConsoleOutput)
            Module.onConsoleOutput(str);
    },

    console_get_size: function(pw, ph)
    {
        var w, h, r;
        r = Module.getConsoleSize ? Module.getConsoleSize() : [80, 30];
        HEAPU32[pw >> 2] = r[0];
        HEAPU32[ph >> 2] = r[1];
    },

    fs_export_file: function(filename, buf, buf_len)
    {
        var _filename = UTF8ToString(filename);
//        console.log("exporting " + _filename);
        var data = HEAPU8.subarray(buf, buf + buf_len);
        var file = new Blob([data], { type: "application/octet-stream" });
        var url = URL.createObjectURL(file);
        var a = document.createElement("a");
        a.href = url;
        a.setAttribute("download", _filename);
        a.innerHTML = "downloading";
        document.body.appendChild(a);
        /* click on the link and remove it */
        setTimeout(function() {
            a.click();
            document.body.removeChild(a);
        }, 50);
    },

    emscripten_async_wget3_data__deps: ['$WgetState'],
    emscripten_async_wget3_data: function(url, request, user, password, post_data, post_data_len, arg, free, onload, onerror, onprogress) {
    var _url = UTF8ToString(url);
    var _request = UTF8ToString(request);
    var _user;
    var _password;

      var http = new XMLHttpRequest();

      if (user)
          _user = UTF8ToString(user);
      else
          _user = null;
      if (password)
          _password = UTF8ToString(password);
      else
          _password = null;
        
      http.open(_request, _url, true);
      http.responseType = 'arraybuffer';
      if (_user) {
          http.setRequestHeader("Authorization", "Basic " + btoa(_user + ':' + _password));
      }
        
    var handle = WgetState.nextHandle++;
    var report = function(phase, loaded, total, status) {
        if (Module.onFetch) {
            Module.onFetch({ id: handle, url: _url, phase: phase,
                             loaded: loaded, total: total, status: status });
        }
    };

    // LOAD
    http.onload = function http_onload(e) {
      /* status 0 with a non-http page means a file:// load; anything
         else that is not 200 (e.g. 404) is an error */
      if (http.status == 200 || (http.status == 0 && location.protocol != "http:" && location.protocol != "https:")) {
        var byteArray = new Uint8Array(http.response);
        report("done", byteArray.length, byteArray.length, http.status);
        var buffer = _malloc(byteArray.length);
        HEAPU8.set(byteArray, buffer);
        if (onload) dynCall('viiii', onload, [handle, arg, buffer, byteArray.length]);
        if (free) _free(buffer);
      } else {
        report("error", 0, 0, http.status);
        if (onerror) dynCall('viiii', onerror, [handle, arg, http.status, http.statusText]);
      }
      delete WgetState.requests[handle];
    };

    // ERROR
    http.onerror = function http_onerror(e) {
      report("error", 0, 0, http.status);
      if (onerror) {
        dynCall('viiii', onerror, [handle, arg, http.status, http.statusText]);
      }
      delete WgetState.requests[handle];
    };

    // PROGRESS
    http.onprogress = function http_onprogress(e) {
      report("progress", e.loaded, e.lengthComputable ? e.total : 0, http.status);
      if (onprogress) dynCall('viiii', onprogress, [handle, arg, e.loaded, e.lengthComputable || e.lengthComputable === undefined ? e.total : 0]);
    };

    // ABORT
    http.onabort = function http_onabort(e) {
      delete WgetState.requests[handle];
    };

    // Useful because the browser can limit the number of redirection
    try {
      if (http.channel instanceof Ci.nsIHttpChannel)
      http.channel.redirectionLimit = 0;
    } catch (ex) { /* whatever */ }

    if (_request == "POST") {
      var _post_data = HEAPU8.subarray(post_data, post_data + post_data_len);
        //Send the proper header information along with the request
      http.setRequestHeader("Content-type", "application/octet-stream");
      http.setRequestHeader("Content-length", post_data_len);
      http.setRequestHeader("Connection", "close");
      http.send(_post_data);
    } else {
      http.send(null);
    }
    report("start", 0, 0, 0);

    WgetState.requests[handle] = http;

    return handle;
  },

  /* Disk persistence: clusters written by the guest are saved in
     IndexedDB and restored before the VM starts. One database holds the
     clusters of a single disk image, keyed by [disk id, cluster number];
     clusters of other disk ids are discarded on open. The page may define
     Module.onDiskPersist(info) to show the state. */
  $DiskPersist: {
      db: null,
      id: null,
      cluster_bytes: 0,
      dirty: {},
      saved: {},
      timer: null,
      n_saved: 0,
      n_restored: 0,
      status: "off",
  },

  $disk_persist_notify__deps: ['$DiskPersist'],
  $disk_persist_notify: function()
  {
      var P = DiskPersist;
      if (Module.onDiskPersist) {
          Module.onDiskPersist({ status: P.status,
                                saved_bytes: P.n_saved * P.cluster_bytes,
                                restored_bytes: P.n_restored * P.cluster_bytes });
      }
  },

  $disk_persist_flush__deps: ['$DiskPersist', '$WgetState', '$disk_persist_notify'],
  $disk_persist_flush: function()
  {
      var P = DiskPersist, dirty, k, data, tx, store, n;
      if (P.timer !== null) {
          clearTimeout(P.timer);
          P.timer = null;
      }
      if (!P.db)
          return;
      dirty = P.dirty;
      P.dirty = {};
      n = 0;
      try {
          tx = P.db.transaction("clusters", "readwrite");
          store = tx.objectStore("clusters");
          for (k in dirty) {
              data = WgetState.fbuf_table[dirty[k]];
              if (data) {
                  store.put(data.slice(0), [P.id, k | 0]);
                  n++;
              }
          }
          tx.oncomplete = function() {
              disk_persist_notify();
          };
          tx.onerror = function() {
              console.log("disk persistence: write failed: " + tx.error);
              P.status = "error";
              disk_persist_notify();
          };
      } catch (e) {
          console.log("disk persistence: " + e);
          P.status = "error";
          disk_persist_notify();
      }
  },

  block_persist_dirty__deps: ['$DiskPersist', '$disk_persist_flush'],
  block_persist_dirty: function(cluster_num, fbuf_handle)
  {
      var P = DiskPersist;
      if (!P.db)
          return;
      P.dirty[cluster_num] = fbuf_handle;
      if (!(cluster_num in P.saved)) {
          P.saved[cluster_num] = 1;
          P.n_saved++;
      }
      if (P.timer === null) {
          P.timer = setTimeout(disk_persist_flush, 300);
          disk_persist_notify();
      }
  },

  block_persist_open__deps: ['$DiskPersist', '$disk_persist_flush', '$disk_persist_notify'],
  block_persist_open: function(bf, disk_id, cluster_bytes)
  {
      var P = DiskPersist, id, req, done, finished = false;

      id = UTF8ToString(disk_id);
      P.cluster_bytes = cluster_bytes;
      done = function(status) {
          if (finished)
              return;
          finished = true;
          P.status = status;
          disk_persist_notify();
          _block_persist_ready(bf);
      };
      if (typeof indexedDB == "undefined") {
          done("off");
          return;
      }
      try {
          req = indexedDB.open("tinyemu-disk", 1);
      } catch (e) {
          done("off");
          return;
      }
      req.onupgradeneeded = function() {
          var db = req.result;
          if (!db.objectStoreNames.contains("clusters"))
              db.createObjectStore("clusters");
      };
      req.onerror = function() {
          console.log("disk persistence unavailable: " + req.error);
          done("off");
      };
      req.onblocked = function() {
          console.log("disk persistence: database blocked");
          done("off");
      };
      req.onsuccess = function() {
          var db = req.result, tx, store, cur, buf, n = 0;
          try {
              tx = db.transaction("clusters", "readwrite");
          } catch (e) {
              done("off");
              return;
          }
          store = tx.objectStore("clusters");
          buf = _malloc(cluster_bytes);
          cur = store.openCursor();
          cur.onsuccess = function() {
              var c = cur.result, key;
              if (!c)
                  return;
              key = c.key;
              if (!(key instanceof Array) || key[0] !== id ||
                  !(c.value instanceof Uint8Array) ||
                  c.value.length != cluster_bytes) {
                  /* another disk image, or garbage */
                  c.delete();
              } else {
                  HEAPU8.set(c.value, buf);
                  _block_persist_set_cluster(bf, key[1], buf);
                  P.saved[key[1]] = 1;
                  n++;
              }
              c.continue();
          };
          tx.oncomplete = function() {
              _free(buf);
              P.db = db;
              P.id = id;
              P.n_restored = n;
              P.n_saved = n;
              db.onversionchange = function() {
                  /* another tab wants to delete the database */
                  disk_persist_flush();
                  db.close();
                  P.db = null;
                  P.status = "off";
                  disk_persist_notify();
              };
              /* do not lose the last second of writes when the tab goes away */
              document.addEventListener("visibilitychange", function() {
                  if (document.visibilityState == "hidden")
                      disk_persist_flush();
              });
              window.addEventListener("pagehide", disk_persist_flush);
              console.log("disk persistence: restored " + n + " clusters for " + id);
              done("on");
          };
          tx.onerror = function() {
              _free(buf);
              console.log("disk persistence: restore failed: " + tx.error);
              done("off");
          };
      };
  },

  fs_wget_update_downloading: function (flag)
  {
      if (Module.onDownloading)
          Module.onDownloading(Boolean(flag));
  },

  block_net_info: function(url, block_bytes, n_blocks, n_zero, n_prefetch)
  {
      if (Module.onDiskInfo) {
          Module.onDiskInfo({ url: UTF8ToString(url), blockBytes: block_bytes,
                              blocks: n_blocks, zeroBlocks: n_zero,
                              prefetchBlocks: n_prefetch });
      }
  },
    
  fb_refresh: function(opaque, data, x, y, w, h, stride)
  {
      var i, j, v, src, image_data, dst_pos, display, dst_pos1, image_stride;

      display = graphic_display;
      /* current x = 0 and w = width for all refreshes */
//      console.log("fb_refresh: x=" + x + " y=" + y + " w=" + w + " h=" + h);
      image_data = display.image.data;
      image_stride = display.width * 4;
      dst_pos1 = (y * display.width + x) * 4;
      for(i = 0; i < h; i = (i + 1) | 0) {
          src = data;
          dst_pos = dst_pos1;
          for(j = 0; j < w; j = (j + 1) | 0) {
              v = HEAPU32[src >> 2];
              image_data[dst_pos] = (v >> 16) & 0xff;
              image_data[dst_pos + 1] = (v >> 8) & 0xff;
              image_data[dst_pos + 2] = v & 0xff;
              image_data[dst_pos + 3] = 0xff; /* XXX: do it once */
              src = (src + 4) | 0;
              dst_pos = (dst_pos + 4) | 0;
          }
          data = (data + stride) | 0;
          dst_pos1 = (dst_pos1 + image_stride) | 0;
      }
      display.ctx.putImageData(display.image, 0, 0, x, y, w, h);
  },

  net_recv_packet: function(bs, buf, buf_len)
  {
      if (Module.onNetPacket) {
          /* a copy: the adapter may keep it after the call returns */
          Module.onNetPacket(HEAPU8.slice(buf, buf + buf_len));
      }
  },

  /* file buffer API */
  file_buffer_get_new_handle__deps: ['$WgetState'],
  file_buffer_get_new_handle: function()
  {
      var h;
      if (typeof WgetState.fbuf_table == "undefined") {
          WgetState.fbuf_table = new Object();
          WgetState.fbuf_next_handle = 1;
      }
      for(;;) {
          h = WgetState.fbuf_next_handle;
          WgetState.fbuf_next_handle++;
          if (WgetState.fbuf_next_handle == 0x80000000)
              WgetState.fbuf_next_handle = 1;
          if (typeof WgetState.fbuf_table[h] == "undefined") {
//              console.log("new handle=" + h);
              return h;
          }
      }
  },
    
  file_buffer_init: function(bs)
  {
      var h;
      HEAPU32[bs >> 2] = 0;
      HEAPU32[(bs + 4) >> 2] = 0;
  },

  file_buffer_resize__deps: ['file_buffer_get_new_handle'],
  file_buffer_resize: function(bs, new_size)
  {
      var h, size, new_data, size1, i, data;
      h = HEAPU32[bs >> 2];
      size = HEAPU32[(bs + 4) >> 2];
      if (new_size == 0) {
          if (h != 0) {
              delete WgetState.fbuf_table[h];
              h = 0;
          }
      } else if (size == 0) {
          h = _file_buffer_get_new_handle();
          new_data = new Uint8Array(new_size);
          WgetState.fbuf_table[h] = new_data;
      } else if (size != new_size) {
          data = WgetState.fbuf_table[h];
          new_data = new Uint8Array(new_size);
          if (new_size > size) {
              new_data.set(data, 0);
          } else {
              for(i = 0; i < new_size; i = (i + 1) | 0)
                  new_data[i] = data[i];
          }
          WgetState.fbuf_table[h] = new_data;
      }
      HEAPU32[bs >> 2] = h;
      HEAPU32[(bs + 4) >> 2] = new_size;
      return 0;
  },
  
  file_buffer_reset: function(bs)
  {
      _file_buffer_resize(bs, 0);
      _file_buffer_init(bs);
  },
    
  file_buffer_write: function(bs, offset, buf, size)
  {
      var h, data, i;
      h = HEAPU32[bs >> 2];
      if (h) {
          data = WgetState.fbuf_table[h];
          for(i = 0; i < size; i = (i + 1) | 0) {
              data[offset + i] = HEAPU8[buf + i];
          }
      }
  },
    
  file_buffer_read: function(bs, offset, buf, size)
  {
      var h, data, i;
      h = HEAPU32[bs >> 2];
      if (h) {
          data = WgetState.fbuf_table[h];
          for(i = 0; i < size; i = (i + 1) | 0) {
              HEAPU8[buf + i] = data[offset + i];
          }
      }
  },

  file_buffer_set: function(bs, offset, val, size)
  {
      var h, data, i;
      h = HEAPU32[bs >> 2];
      if (h) {
          data = WgetState.fbuf_table[h];
          for(i = 0; i < size; i = (i + 1) | 0) {
              data[offset + i] = val;
          }
      }
  },
   

  /* Snapshots: a saved machine that the page can restore instead of
     booting. The file (gzip-compressed when served):
       "TEMUSNAP", u32 version (1)
       u32 length, machine state (vm_snapshot_save_state)
       u32 length, extra data for the page (Module.snapshotExtra when
         saved, e.g. the console output so far)
       u32 region count, then per RAM region: u32 size, u32 page size,
         a bitmap of the pages that are not all zeros, those pages
     All numbers little-endian. Module.onSnapshot(info) follows a restore:
     phase "start", "progress" (loaded, total: bytes downloaded),
     "restored" (extra), "skipped" or "error" (message; the machine boots
     instead). Module.snapshot = false skips it. */
  snapshot_write: function(state, state_len)
  {
      var cb = Module.onSnapshotSaved, PAGE = 4096;
      var extra, regions = [], idx, ptr, size, sizep, total, r, i, j, n;
      var out, dv, pos, base, nonzero;
      if (state_len < 0) {
          if (cb)
              cb(null);
          return;
      }
      extra = Module.snapshotExtra || new Uint8Array(0);
      sizep = _malloc(4);
      for(idx = 0; ; idx++) {
          ptr = _vm_snapshot_get_ram(idx, sizep);
          if (!ptr)
              break;
          regions.push({ ptr: ptr, size: HEAP32[sizep >> 2] });
      }
      _free(sizep);
      total = 8 + 4 + 4 + state_len + 4 + extra.length + 4;
      for(idx = 0; idx < regions.length; idx++) {
          r = regions[idx];
          n = r.size / PAGE;
          r.bitmap = new Uint8Array((n + 7) >> 3);
          r.count = 0;
          for(i = 0; i < n; i++) {
              base = (r.ptr + i * PAGE) >> 2;
              nonzero = false;
              for(j = 0; j < PAGE / 4; j++) {
                  if (HEAP32[base + j] != 0) {
                      nonzero = true;
                      break;
                  }
              }
              if (nonzero) {
                  r.bitmap[i >> 3] |= 1 << (i & 7);
                  r.count++;
              }
          }
          total += 8 + r.bitmap.length + r.count * PAGE;
      }
      out = new Uint8Array(total);
      dv = new DataView(out.buffer);
      pos = 0;
      for(i = 0; i < 8; i++)
          out[pos++] = "TEMUSNAP".charCodeAt(i);
      dv.setUint32(pos, 1, true); pos += 4;
      dv.setUint32(pos, state_len, true); pos += 4;
      out.set(HEAPU8.subarray(state, state + state_len), pos); pos += state_len;
      dv.setUint32(pos, extra.length, true); pos += 4;
      out.set(extra, pos); pos += extra.length;
      dv.setUint32(pos, regions.length, true); pos += 4;
      for(idx = 0; idx < regions.length; idx++) {
          r = regions[idx];
          dv.setUint32(pos, r.size, true); pos += 4;
          dv.setUint32(pos, PAGE, true); pos += 4;
          out.set(r.bitmap, pos); pos += r.bitmap.length;
          n = r.size / PAGE;
          for(i = 0; i < n; i++) {
              if (r.bitmap[i >> 3] & (1 << (i & 7))) {
                  out.set(HEAPU8.subarray(r.ptr + i * PAGE, r.ptr + (i + 1) * PAGE), pos);
                  pos += PAGE;
              }
          }
      }
      if (cb)
          cb(out, { regions: regions.map(function(r) {
              return { size: r.size, pages: r.count };
          }) });
  },

  disk_wait_wanted: function()
  {
      return Module.waitForDisk === false ? 0 : 1;
  },

  vm_wait_notify: function(disk, snapshot)
  {
      if (Module.onWaiting)
          Module.onWaiting({ disk: !!disk, snapshot: !!snapshot });
  },

  vm_run_notify: function(restored)
  {
      if (Module.onRun)
          Module.onRun({ restored: !!restored });
  },

  snapshot_fetch: function(url_ptr, size)
  {
      var url = UTF8ToString(url_ptr);
      var touched = false, state_ptr = 0, loaded = 0, total = size;

      function report(info)
      {
          info.url = url;
          if (Module.onSnapshot)
              Module.onSnapshot(info);
      }

      /* read exactly n bytes at a time from a stream of Uint8Arrays */
      function ByteReader(reader)
      {
          var chunk = null, off = 0;
          this.read = async function(n) {
              var out = null, pos = 0, avail, l, v, res;
              while (pos < n) {
                  if (!chunk || off >= chunk.length) {
                      res = await reader.read();
                      if (res.done)
                          throw new Error("the snapshot is truncated");
                      chunk = res.value;
                      off = 0;
                  }
                  avail = chunk.length - off;
                  if (pos == 0 && avail >= n) {
                      v = chunk.subarray(off, off + n);
                      off += n;
                      return v;
                  }
                  if (!out)
                      out = new Uint8Array(n);
                  l = Math.min(avail, n - pos);
                  out.set(chunk.subarray(off, off + l), pos);
                  off += l;
                  pos += l;
              }
              return out;
          };
          this.u32 = async function() {
              var b = await this.read(4);
              return (b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)) >>> 0;
          };
      }

      async function restore()
      {
          var resp, body, body_reader, first, gz, stream, r, b, i, n, state, extra;
          var n_regions, idx, size, page, bitmap, ptr, sizep, actual, last = 0;

          resp = await fetch(url);
          if (!resp.ok)
              throw new Error("HTTP " + resp.status);
          total = +(resp.headers.get("Content-Length") || 0) || size;
          body = resp.body.pipeThrough(new TransformStream({
              transform: function(chunk, ctl) {
                  var now = performance.now();
                  loaded += chunk.length;
                  if (now - last >= 100) {
                      last = now;
                      report({ phase: "progress", loaded: loaded, total: total });
                  }
                  ctl.enqueue(chunk);
              }
          }));
          /* gzip, unless the server already decoded it
             (Content-Encoding) */
          body_reader = body.getReader();
          first = await body_reader.read();
          if (first.done)
              throw new Error("the snapshot is empty");
          gz = first.value.length >= 2 && first.value[0] == 0x1f &&
              first.value[1] == 0x8b;
          stream = new ReadableStream({
              start: function(ctl) { ctl.enqueue(first.value); },
              pull: async function(ctl) {
                  var res = await body_reader.read();
                  if (res.done)
                      ctl.close();
                  else
                      ctl.enqueue(res.value);
              }
          });
          if (gz)
              stream = stream.pipeThrough(new DecompressionStream("gzip"));
          r = new ByteReader(stream.getReader());

          b = await r.read(12);
          for(i = 0; i < 8; i++) {
              if (b[i] != "TEMUSNAP".charCodeAt(i))
                  throw new Error("not a snapshot");
          }
          if ((b[8] | (b[9] << 8) | (b[10] << 16) | (b[11] << 24)) != 1)
              throw new Error("unsupported snapshot version");
          n = await r.u32();
          state = await r.read(n);
          state_ptr = _malloc(n);
          HEAPU8.set(state, state_ptr);
          if (_vm_snapshot_load_state(state_ptr, n, 1) != 0)
              throw new Error("the snapshot does not fit this machine");
          n = await r.u32();
          extra = (await r.read(n)).slice(0);

          n_regions = await r.u32();
          sizep = _malloc(4);
          for(idx = 0; idx < n_regions; idx++) {
              size = await r.u32();
              page = await r.u32();
              ptr = _vm_snapshot_get_ram(idx, sizep);
              actual = HEAP32[sizep >> 2];
              if (!ptr || size != actual || page != 4096 || size % page) {
                  _free(sizep);
                  throw new Error("the snapshot's memory does not fit this machine");
              }
              bitmap = (await r.read((size / page + 7) >> 3)).slice(0);
              touched = true;
              HEAPU8.fill(0, ptr, ptr + size);
              n = size / page;
              for(i = 0; i < n; i++) {
                  if (bitmap[i >> 3] & (1 << (i & 7)))
                      HEAPU8.set(await r.read(page), ptr + i * page);
              }
          }
          _free(sizep);
          if (_vm_snapshot_load_state(state_ptr, state.length, 0) != 0)
              throw new Error("the snapshot could not be loaded");
          _free(state_ptr);
          state_ptr = 0;
          return extra;
      }

      if (Module.snapshot === false || typeof fetch == "undefined" ||
          typeof DecompressionStream == "undefined" ||
          typeof TransformStream == "undefined") {
          report({ phase: "skipped",
                   message: Module.snapshot === false ? "disabled" :
                   "this browser cannot decompress it" });
          _vm_snapshot_done(0);
          return;
      }
      report({ phase: "start", loaded: 0, total: size });
      restore().then(function(extra) {
          report({ phase: "restored", loaded: loaded, total: total,
                   extra: extra });
          _vm_snapshot_done(1);
      }, function(e) {
          if (state_ptr)
              _free(state_ptr);
          console.log("snapshot: " + (e && e.message ? e.message : e));
          report({ phase: "error", message: String(e && e.message ? e.message : e) });
          _vm_snapshot_done(touched ? -1 : 0);
      });
  },
});
