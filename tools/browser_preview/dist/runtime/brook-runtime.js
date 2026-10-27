import {createMonitorPty} from './monitor-pty.js';

/** Brook runs as a real x86-64 QEMU guest. No remote execution or guest network. */
export async function startBrook({canvas, assets, print = console.log, status = () => {}, onError = () => {}, monitor = false, monitorOob = false, monitorSamples = true, pollFix = false, chainFix = false, exitFix = false, fullNoChain = false, interpreterOnly = false, onQmp = () => {}, extraArguments = []}) {
  if (!globalThis.crossOriginIsolated || typeof SharedArrayBuffer === 'undefined') {
    throw new Error('This emulator needs cross-origin isolation (COOP and COEP headers).');
  }
  const base = new URL('./', import.meta.url);
  const runtime = await fetch(new URL(fullNoChain?'runtime-nochain.json':'runtime.json', base)).then(response => {
    if (!response.ok) throw new Error(`Runtime manifest: HTTP ${response.status}`);
    return response.json();
  });
  async function download(url) {
    const response = await fetch(url);
    if (!response.ok) throw new Error(`Asset download: HTTP ${response.status} (${url})`);
    return new Uint8Array(await response.arrayBuffer());
  }
  status('Downloading emulator and Brook boot image…');
  const chunks = [];
  for (const [index, part] of runtime.parts.entries()) {
    chunks.push(await download(new URL(part.file, base)));
    status(`Downloaded emulator part ${index + 1}/${runtime.parts.length}`);
  }
  const wasmBinary = new Uint8Array(chunks.reduce((sum, part) => sum + part.length, 0));
  let offset = 0;
  for (const part of chunks) {
    wasmBinary.set(part, offset);
    offset += part.length;
  }
  chunks.length = 0;
  const files = [];
  for (const asset of assets) {
    let bytes = await download(asset.url);
    if (asset.compression && asset.compression !== 'gzip') {
      throw new Error(`Unsupported boot asset compression: ${asset.compression}`);
    }
    if (asset.compression === 'gzip') {
      // Fetch can transparently decode Content-Encoding. Inspect the bytes so
      // either static hosting configuration works without double decompression.
      const isGzip = bytes[0] === 0x1f && bytes[1] === 0x8b;
      if (isGzip) {
        if (typeof DecompressionStream === 'undefined') {
          throw new Error('This browser needs gzip DecompressionStream support to unpack Brook.');
        }
        status('Unpacking the Brook boot image…');
        const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('gzip'));
        bytes = new Uint8Array(await new Response(stream).arrayBuffer());
      } else if (!asset.uncompressedBytes || bytes.length !== asset.uncompressedBytes) {
        throw new Error(`Expected a gzip boot asset: ${asset.url}`);
      }
    }
    if (asset.uncompressedBytes !== undefined && bytes.length !== asset.uncompressedBytes) {
      throw new Error(`Boot asset size mismatch: ${asset.path} (${bytes.length} bytes)`);
    }
    files.push({...asset, bytes});
  }
  status('Starting the x86-64 machine…');
  const runtimeScript=exitFix?'qemu-system-x86_64.exitfix.js':chainFix?'qemu-system-x86_64.chainfix.js':pollFix?'qemu-system-x86_64.pollfix.js':'qemu-system-x86_64.js';
  const {default: createQemu} = await import(new URL(runtimeScript,base).href);
  if(interpreterOnly){
    const digest=Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',wasmBinary)),v=>v.toString(16).padStart(2,'0')).join('');
    if(digest!=='fbb8f2c84b4a6dacdad867709126fce3ae42ab616bd611f26f806a415f49f9f1')throw new Error('Interpreter diagnostic requires the exact pinned runtime');
  }
  const monitorPty=createMonitorPty({enabled:monitor,oob:monitorOob,samples:monitorSamples,print,onMessage:onQmp});
  const instance = await createQemu({
    pty:monitorPty.pty,
    canvas,
    wasmBinary,
    INITIAL_MEMORY: 1024 * 1024 * 1024,
    mainScriptUrlOrBlob: new URL(runtimeScript, base).href,
    locateFile: file => new URL(file, base).href,
    arguments: [
      '-display', 'sdl,gl=off',
      '-cpu', 'max',
      '-m', '384M',
      '-accel', 'tcg,tb-size=64,thread=single',
      '-smp', '1',
      '-L', '/pack',
      '-nic', 'none',
      '-serial', 'file:/pack/serial.log',
      '-monitor', 'none',
      ...(monitor?['-qmp','stdio']:[]),
      '-no-reboot',
      '-no-shutdown',
      '-d', 'guest_errors,cpu_reset',
      '-D', '/pack/qemu-debug.log',
      ...extraArguments,
    ],
    print: (...args) => print(args.join(' ')),
    printErr: (...args) => print(args.join(' ')),
    setStatus: text => { if (text) status(text === 'Running...' ? 'Emulator initialized; waiting for Brook…' : text); },
    onAbort: reason => {
      monitorPty.dispose();
      const error = new Error(`Emulator aborted: ${reason}`);
      status(error.message);onError(error);
    },
    onExit: code => {
      monitorPty.dispose();
      const error = new Error(`Emulator exited (code ${code}).`);
      status(error.message);onError(error);
    },
    preRun: [module => {
      if(interpreterOnly){
        // Exact pinned source/objdump correlation: tcg/wasm32.c
        // can_add_instance reads instance_alive_global at8011316 and compares
        // with MAX_INSTANCE_ALIVE=15000. Holding capacity full from startup
        // selects its existing interpreter fallback; no modules have existed.
        Atomics.store(module.HEAP32,8011316>>>2,15000);
      }
      module.FS.mkdirTree('/pack');
      module.FS.mkdirTree('/tmp');
      module.FS.mkdirTree('/var/tmp');
      for (const file of files) {
        module.FS.writeFile(file.path, file.bytes, {canOwn: true});
        file.bytes = null;
      }
    }],
  });
  instance.brookMonitor=monitorPty;
  return instance;
}
