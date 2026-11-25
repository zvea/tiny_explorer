/**
 * Unified GDS Worker
 * Supports both streaming visualization (viewer.html) and benchmarking (bench.html).
 * Integrates brotli-dec-wasm for .gds.br support.
 */

// MARK: - Imports & Globals
import brotliInit, { DecompressStream, BrotliStreamResultCode } from '../vendor/brotli_dec_wasm.js';
import { WASI, File, PreopenDirectory, Fd, ConsoleStdout } from '../vendor/browser_wasi_shim.js';

let instance;
let logBuffer = "";

function log(msg) {
    postMessage({ type: 'log', message: msg });
}

function progress(msg) {
    postMessage({ type: 'progress', message: msg });
}

async function fetchWithProgress(url, label = "GDS") {
    progress(`Fetching ${label}...`);
    const response = await fetch(url);
    if (!response.ok) {
        throw new Error(`HTTP ${response.status} fetching ${url}`);
    }

    const contentLength = response.headers.get('content-length');
    const total = contentLength ? parseInt(contentLength, 10) : 0;

    if (!response.body || !response.body.getReader) {
        return new Uint8Array(await response.arrayBuffer());
    }

    const reader = response.body.getReader();
    const chunks = [];
    let received = 0;
    let lastReport = performance.now();

    while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        chunks.push(value);
        received += value.length;

        const now = performance.now();
        if (now - lastReport > 100) {
            lastReport = now;
            if (total > 0) {
                const percent = Math.round((received / total) * 100);
                progress(`Fetching ${label}: ${percent}% (${(received / 1024 / 1024).toFixed(1)} / ${(total / 1024 / 1024).toFixed(1)} MB)`);
            } else {
                progress(`Fetching ${label}: ${(received / 1024 / 1024).toFixed(1)} MB downloaded...`);
            }
        }
    }

    if (total > 0) {
        progress(`Fetching ${label}: 100% (${(received / 1024 / 1024).toFixed(1)} MB)`);
    } else {
        progress(`Fetched ${label}: ${(received / 1024 / 1024).toFixed(1)} MB`);
    }

    const result = new Uint8Array(received);
    let offset = 0;
    for (const chunk of chunks) {
        result.set(chunk, offset);
        offset += chunk.length;
    }
    return result;
}

let clkNetId = -1;
let simRunning = false;
let simSpeed = 0;
let autoClock = false;
let lastSimUpdate = 0;

const files = new Map();

// MARK: - WASM Memory Utilities
function allocString(str, instance) {
    const bytes = new TextEncoder().encode(str);
    const ptr = instance.exports.wasm_malloc(bytes.length + 1);
    const buf = new Uint8Array(instance.exports.memory.buffer, ptr, bytes.length + 1);
    buf.set(bytes);
    buf[bytes.length] = 0;
    return ptr;
}

function getString(ptr) {
    if (!ptr) return null;
    const buf = new Uint8Array(instance.exports.memory.buffer);
    let end = ptr;
    while (buf[end]) end++;
    return new TextDecoder().decode(buf.slice(ptr, end));
}

function getWireNames() {
    const count = instance.exports.wasm_labeledCount();
    const labeled = [];
    for (let i = 0; i < count; i++) {
        labeled.push({
            id: instance.exports.wasm_circuit_get_labeled_id(i),
            name: getString(instance.exports.wasm_circuit_get_labeled_name(i))
        });
    }
    return labeled;
}

// MARK: - GDS Processing
async function runGdsTask(gdsUrl, pdk, options = {}) {
    logBuffer = "";
    const { returnGeometry = true } = options;
    
    const targetUrl = options.filename || gdsUrl;
    const urlPath = targetUrl.toLowerCase().split(/[?#]/)[0];
    const isBrotli = urlPath.endsWith('.br');
    const isGzip = urlPath.endsWith('.gz');
    
    try {
        if (isBrotli) await brotliInit();

        // 1. Fetch and decompress data
        const startTime = performance.now();
        const rawBytes = await fetchWithProgress(gdsUrl, isBrotli ? "Brotli GDS" : (isGzip ? "Gzipped GDS" : "GDS"));
        let data;

        if (isGzip) {
            progress("Decompressing GZip layout...");
            const stream = new Response(rawBytes).body.pipeThrough(new DecompressionStream('gzip'));
            data = new Uint8Array(await new Response(stream).arrayBuffer());
        } else if (isBrotli) {
            progress("Decompressing Brotli layout...");
            const brotli = new DecompressStream();
            const result = brotli.decompress(rawBytes, 200 * 1024 * 1024); // max 200MB
            data = result.buf;
        } else {
            data = rawBytes;
        }

        const totalBytes = data.length;
        const virtPath = urlPath.substring(urlPath.lastIndexOf('/') + 1).replace('.gz', '').replace('.br', '');

        // 2. Setup WASI
        const stdoutHandler = bytes => {
            const text = new TextDecoder().decode(bytes);
            logBuffer += text;
            log(text);
        };

        const fds = [
            new Fd(),
            new ConsoleStdout(stdoutHandler),
            new ConsoleStdout(stdoutHandler),
            new PreopenDirectory(".", new Map([[virtPath, new File(data)]]))
        ];

        const wasi = new WASI(["explorer"], [], fds);

        // 3. Initialize WASM
        progress("Initializing WASM engine...");
        const wasmResponse = await fetch('explorer.wasm');
        const wasmBuffer = await wasmResponse.arrayBuffer();
        const { instance: wasmInstance } = await WebAssembly.instantiate(wasmBuffer, {
            wasi_snapshot_preview1: wasi.wasiImport
        });
        instance = wasmInstance;
        
        wasi.initialize(instance);
        if (instance.exports.wasm_arena_init) instance.exports.wasm_arena_init(1); // Enable Arena Mode (1 = Arena, 0 = Heap)

        // 4. GDSTK Load phase
        progress("Parsing layout with GDSTK...");
        const loadStart = performance.now();
        const pathPtr = allocString(virtPath, instance);
        const pdkPtr = allocString(pdk || "", instance);

        instance.exports.wasm_load_file(pathPtr, pdkPtr);
        instance.exports.wasm_free(pathPtr);
        instance.exports.wasm_free(pdkPtr);
        const loadTime = performance.now() - loadStart;

        // 5. Processing phase
        progress("Extracting circuit topology & hierarchy...");
        const procStart = performance.now();

        instance.exports.wasm_process();
        const procTime = performance.now() - procStart;
        
        const totalTime = performance.now() - startTime;
        const memMB = instance.exports.memory.buffer.byteLength / 1024 / 1024;

        log(`\n--- Stats ---\n`);
        log(`Load time: ${loadTime.toFixed(2)}ms\n`);
        log(`Process time: ${procTime.toFixed(2)}ms\n`);
        log(`Total wall time: ${totalTime.toFixed(2)}ms\n`);
        log(`Memory usage: ${memMB.toFixed(2)}MB\n`);

        const stats = {
            totalBytes,
            totalTime,
            loadTime,
            procTime,
            wallTime: totalTime,
            memMB: memMB.toFixed(2),
            peakMemMB: memMB.toFixed(2),
            flatRects: parseInt(logBuffer.match(/Total flat rects: (\d+)/)?.[1] || 0),
            flatFETs: 0, // Not currently explicitly logged
            netlistWires: parseInt(logBuffer.match(/Wires: (\d+)/)?.[1] || 0),
            netlistFETs: parseInt(logBuffer.match(/FETs: (\d+)/)?.[1] || 0),
            warnings: (logBuffer.match(/Warning:/g) || []).length,
            errors: (logBuffer.match(/Error:/g) || []).length,
        };

        const transferables = [];
        if (returnGeometry) {
            console.log(instance.exports);
            const rectDataPtr = instance.exports.wasm_flatRects_ptr();
            const rectDataSize = instance.exports.wasm_flatRects_size();
            const layerOffsetsPtr = instance.exports.wasm_flatLayerOffsets_ptr();
            const layerOffsetsSize = instance.exports.wasm_flatLayerOffsets_size();

            if (rectDataPtr && rectDataSize) {
                // rectDataSize is in bytes, Int32Array expects element count
                const rectData = new Int32Array(instance.exports.memory.buffer, rectDataPtr, rectDataSize / 4).slice();
                stats.rectData = rectData;
                transferables.push(rectData.buffer);
            }
            if (layerOffsetsPtr && layerOffsetsSize) {
                // layerOffsetsSize is in bytes, Uint32Array expects element count
                const layerOffsets = new Uint32Array(instance.exports.memory.buffer, layerOffsetsPtr, layerOffsetsSize / 4).slice();
                stats.layerOffsets = layerOffsets;
                transferables.push(layerOffsets.buffer);
            }
        }

        const wireNames = getWireNames();
        clkNetId = -1;
        wireNames.forEach(w => {
            if (/^clk$/i.test(w.name)) clkNetId = w.id;
        });
        instance.exports.wasm_sim_set_clock(clkNetId);

        postMessage({ type: 'done', stats, file: gdsUrl, pdk, wireNames }, transferables);

    } catch (err) {
        postMessage({ type: 'error', file: gdsUrl, message: err.message });
    }
}

// MARK: - Worker Message Loop
onmessage = function(e) {
    if (e.data.type === 'start') {
        runGdsTask(e.data.gdsUrl, e.data.pdk, e.data.options || {});
    } else if (e.data.type === 'call') {
        const { name, args, returnArrays } = e.data;
        const fn = instance.exports[name];
        if (!fn) {
            postMessage({ type: 'error', message: `WASM function not found: ${name}` });
            return;
        }
        const result = fn(...(args || []));

        const payload = { type: 'callResult', name, result };
        if (instance.exports.wasm_circuit_is_settled) {
            payload.isSettled = !!instance.exports.wasm_circuit_is_settled();
        }
        const transferables = [];        
        if (returnArrays) {
            returnArrays.forEach(arrName => {
                const getPtr = instance.exports[`wasm_${arrName}_ptr`];
                const getSize = instance.exports[`wasm_${arrName}_size`];
                if (getPtr && getSize) {
                    const ptr = getPtr();
                    if (ptr) {
                        const count = getSize();
                        const data = new Uint8Array(instance.exports.memory.buffer, ptr, count).slice();
                        payload[arrName] = data;
                        transferables.push(data.buffer);
                    }
                }
            });
        }
        postMessage(payload, transferables);
    } else if (e.data.type === 'sim_config') {
        const { speed, autoClock: autoClk } = e.data;
        simSpeed = speed;
        autoClock = autoClk;
        
        const shouldRun = simSpeed > 0 || autoClock;
        if (shouldRun && !simRunning) {
            simRunning = true;
            lastSimLoopTime = performance.now();
            waveAccumulator = 0;
            scheduleNext(simLoop);
        } else if (!shouldRun) {
            if (simRunning) {
                simRunning = false;
                sendSimUpdate(true); // Final sync when stopping
            }
        }
    } else if (e.data.type === 'frame') {
        frameRequested = true;
    } else if (e.data.type === 'visibility') {
        pageHidden = e.data.hidden;
        if (!pageHidden && parkedCallback) {
            const cb = parkedCallback;
            parkedCallback = null;
            scheduleNext(cb);
        }
    } else if (e.data.type === 'ack_request') {
        postMessage({ type: 'ack_response' });
    }
};

// MARK: - Simulation Subsystem
let waveAccumulator = 0;
let lastSimLoopTime = 0;
let wireDataNeedsClear = false;

// At full speed the loop runs again as soon as it yields (a MessageChannel message, which
// browsers do not throttle like timers). Slower speeds, and any speed while the page is hidden,
// follow the display's frames, which browsers pause for hidden pages.
let pageHidden = false;
let frameRequested = false;  // the page is ready to show a new state (full speed only)
let unsentWork = false;      // simulated since the last update was posted (full speed only)
let parkedCallback = null;  // the loop, parked while hidden where there is no requestAnimationFrame
const loopChannel = new MessageChannel();
let loopCallback = null;
loopChannel.port1.onmessage = () => {
    const cb = loopCallback;
    loopCallback = null;
    if (cb) cb();
};

function scheduleNext(cb) {
    if (simSpeed === 100 && !pageHidden) {
        loopCallback = cb;
        loopChannel.port2.postMessage(0);
    } else if (typeof requestAnimationFrame !== 'undefined') {
        requestAnimationFrame(cb);
    } else if (pageHidden) {
        parkedCallback = cb;
    } else {
        setTimeout(cb, 0);
    }
}

function sendSimUpdate(forceWireData = false) {
    const payload = { type: 'simUpdate' };
    if (instance.exports.wasm_sim_clock_count) {
        payload.clockCount = instance.exports.wasm_sim_clock_count() >>> 0;
        payload.clockTime = performance.now();  // timed here: the page may handle it late
    }
    const transferables = [];
    
    if (forceWireData && instance.exports.wasm_wireData_ptr) {
        const data = new Uint8Array(instance.exports.memory.buffer, instance.exports.wasm_wireData_ptr(), instance.exports.wasm_wireData_size()).slice();
        payload.wireData = data;
        if (instance.exports.wasm_circuit_is_settled) {
            payload.isSettled = !!instance.exports.wasm_circuit_is_settled();
        }
        transferables.push(data.buffer);
    }
    
    if (instance.exports.wasm_vga_buffer_ptr) {
        const ex = instance.exports;
        const stride = ex.wasm_vga_stride();
        const vga = {
            stride,
            width: ex.wasm_vga_width(),
            height: ex.wasm_vga_height(),
            rayX: ex.wasm_vga_ray_x(),
            rayY: ex.wasm_vga_ray_y(),
        };
        // Only the lines drawn since the last update; all of them after the buffer was cleared.
        const full = ex.wasm_vga_dirty_full();
        const y0 = full ? 0 : ex.wasm_vga_dirty_y0();
        const y1 = full ? vga.height - 1 : ex.wasm_vga_dirty_y1();
        ex.wasm_vga_clear_dirty();
        if (y1 >= y0) {
            const rows = new Uint8Array(ex.memory.buffer, ex.wasm_vga_buffer_ptr() + y0 * stride * 3,
                                        (y1 - y0 + 1) * stride * 3).slice();
            vga.rows = rows;
            vga.y0 = y0;
            transferables.push(rows.buffer);
        }
        payload.vga = vga;
    }
    
    postMessage(payload, transferables);
    lastSimUpdate = performance.now();
}

// Steps per call into WASM, adapted so that a call takes about CALL_TARGET_MS on the loaded
// design and this machine: a step's cost varies with the design's size and activity. The
// target sits well under the loop's 2 ms budget at slow speeds, and far above a call's overhead.
const CALL_TARGET_MS = 1;
let stepsPerCall = 16;

function adaptStepsPerCall(ms) {
    // At most doubling per call, as coarse timers can read 0 ms.
    const factor = ms > 0 ? Math.min(2, CALL_TARGET_MS / ms) : 2;
    stepsPerCall = Math.max(1, Math.min(1 << 20, Math.round(stepsPerCall * factor)));
}

function simLoop() {
    if (!simRunning) return;
    
    if (!instance || !instance.exports.wasm_circuit_is_settled) {
        scheduleNext(simLoop);
        return;
    }

    const now = performance.now();
    const isMax = simSpeed === 100;
    // Work in short slices, so that at full speed a frame the page asks for is answered promptly.
    const timeLimit = now + 2;
    
    let iterCount = 0;
    
    if (isMax) {
        iterCount = 100000;
    } else if (simSpeed > 0) {
        // Clamp delta to prevent massive jumps if tab was sleeping
        const delta = Math.min(now - lastSimLoopTime, 100);
        
        // Logarithmic scale so speed=1 => 1 wps, speed=99 => 10000 wps
        const targetWavesPerSec = Math.pow(10, ((simSpeed - 1) / 98) * 4);
        waveAccumulator += (delta / 1000.0) * targetWavesPerSec;
        
        iterCount = Math.floor(waveAccumulator);
        if (iterCount > 0) {
            waveAccumulator -= iterCount;
        }
    } else if (autoClock) {
        // Speed slider is 0 (Paused) but autoClock is manually on
        // Leave logic iterCount at 0 so only clock edges run
        iterCount = 0;
    }
    
    lastSimLoopTime = now;

    // Steps (settling waves, or clock toggles once settled) run in WASM, a chunk per call,
    // with the time checked between chunks.
    let didWork = false;
    while (iterCount > 0 && performance.now() < timeLimit) {
        const asked = Math.min(iterCount, stepsPerCall);
        const callStart = performance.now();
        const done = instance.exports.wasm_sim_run(asked, autoClock ? 1 : 0);
        if (done === 0) break; // Settled and no clock to toggle
        if (asked === stepsPerCall && done === asked) adaptStepsPerCall(performance.now() - callStart);
        didWork = true;
        iterCount -= done;
    }
    
    const forceResync = (now - lastSimUpdate) > 100;
    const isSettled = instance.exports.wasm_circuit_is_settled();
    
    if (didWork) {
        wireDataNeedsClear = true;
    }
    
    let sendWireData = didWork;
    if (!didWork && forceResync && isSettled && wireDataNeedsClear) {
        sendWireData = true;
        wireDataNeedsClear = false;
    }
    
    if (isMax) {
        // Once per frame the page displays, in step with it
        if (didWork) unsentWork = true;
        if (frameRequested && (unsentWork || forceResync)) {
            sendSimUpdate(sendWireData || unsentWork);
            frameRequested = false;
            unsentWork = false;
        }
    } else if (didWork || forceResync) {
        // limit UI updates to ~60fps (16ms)
        // BUT: If the circuit is settled and we just did work, this is a 'final' state, send it immediately
        if ((now - lastSimUpdate) >= 16 || (isSettled && didWork)) {
            sendSimUpdate(sendWireData);
        }
    }
    
    scheduleNext(simLoop);
}
