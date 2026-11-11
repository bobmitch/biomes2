// Track A, milestone A1 -- a top-down map of the world disc, driven by the
// canonical field function.
//
// The WGSL under src/generated/ is a BUILD PRODUCT emitted by tools/wgslgen
// from worldgen/shaders/. Nothing here may define terrain: if a number that
// shapes the world appears in this file, it belongs in the dialect instead.

import noiseWgsl from './generated/noise.wgsl?raw';
import fieldWgsl from './generated/field.wgsl?raw';
import kernelWgsl from './kernel.wgsl?raw';
import blitWgsl from './blit.wgsl?raw';

const RES = 2048;

// World parameters, matching tools/spike's defaults so the two can be compared
// directly. These move into a shared data file when A2 brings the tuning UI.
const WORLD = {
  radius: 10000,
  continentFreq: 1 / 5200,
  centreBias: 0.7,
  edgeDrop: 1.6,
  landOffset: 0.17,
  dangerW: 0.2,
  dangerFreq: 1 / 4500,
  seaLevel: 0,
  continentOctaves: 5,
  dangerOctaves: 3,
};

// Per-feature seeds: hash64(world_seed ^ FEATURE_ID). This mirrors
// tools/spike/host_support.h deliberately -- it is host-side seed derivation,
// not field evaluation, and WGSL has no 64-bit integers to do it with.
const MASK64 = (1n << 64n) - 1n;

function hash64(x: bigint): bigint {
  x = (x + 0x9e3779b97f4a7c15n) & MASK64;
  let z = x;
  z = ((z ^ (z >> 30n)) * 0xbf58476d1ce4e5b9n) & MASK64;
  z = ((z ^ (z >> 27n)) * 0x94d049bb133111ebn) & MASK64;
  return (z ^ (z >> 31n)) & MASK64;
}

const FEATURE_CONTINENT = 1n;
const FEATURE_DANGER = 2n;

function featureSeed(worldSeed: bigint, featureId: bigint): number {
  return Number(hash64((worldSeed ^ featureId) & MASK64) >> 32n) >>> 0;
}

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

function fail(message: string): never {
  const box = $('error');
  box.style.display = 'block';
  box.textContent = message;
  $('map').style.display = 'none';
  throw new Error(message);
}

async function main() {
  if (!navigator.gpu) {
    fail(
      'WebGPU is not available in this browser.\n\n' +
        'The map view renders the field function in a compute shader, so there is no ' +
        'fallback path. Chrome 113+, Edge 113+ or Safari 18+ will work.',
    );
  }

  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) fail('No WebGPU adapter. A software adapter would do, but none was offered.');
  const device = await adapter.requestDevice();

  device.addEventListener('uncapturederror', (e) => {
    // Shader compile failures surface here as well as on the promise, and a
    // generator bug is exactly the kind of thing that shows up this way.
    console.error('WebGPU error:', (e as GPUUncapturedErrorEvent).error.message);
  });

  const canvas = $<HTMLCanvasElement>('map');
  const ctx = canvas.getContext('webgpu');
  if (!ctx) fail('Could not acquire a WebGPU canvas context.');
  const format = navigator.gpu.getPreferredCanvasFormat();
  ctx.configure({ device, format, alphaMode: 'opaque' });

  // The field function comes first; the kernel calls into it. Concatenated
  // rather than imported because WGSL has no module system.
  const computeSource = `${noiseWgsl}\n${fieldWgsl}\n${kernelWgsl}`;

  device.pushErrorScope('validation');
  const computeModule = device.createShaderModule({ code: computeSource, label: 'field+kernel' });
  const blitModule = device.createShaderModule({ code: blitWgsl, label: 'blit' });

  // Compilation messages are the only check we have that the generated WGSL
  // is well formed. Report them loudly rather than letting a blank canvas
  // stand in for a generator bug.
  for (const [name, mod] of [
    ['field+kernel', computeModule],
    ['blit', blitModule],
  ] as const) {
    const info = await mod.getCompilationInfo();
    const errors = info.messages.filter((m) => m.type === 'error');
    for (const m of info.messages) {
      const where = `${name}:${m.lineNum}:${m.linePos}`;
      if (m.type === 'error') console.error(`${where} ${m.message}`);
      else console.warn(`${where} ${m.message}`);
    }
    if (errors.length > 0) {
      fail(
        `The generated WGSL did not compile (${name}).\n\n` +
          errors.map((m) => `line ${m.lineNum}:${m.linePos}  ${m.message}`).join('\n\n') +
          '\n\nThis is a tools/wgslgen bug, not a page bug. Do not hand-edit src/generated/.',
      );
    }
  }
  await device.popErrorScope();

  const texture = device.createTexture({
    size: [RES, RES],
    format: 'rgba8unorm',
    usage: GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.TEXTURE_BINDING,
  });

  const uniformBuf = device.createBuffer({
    size: 64,
    usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
  });
  const countsBuf = device.createBuffer({
    size: 8,
    usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST,
  });
  const readbackBuf = device.createBuffer({
    size: 8,
    usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST,
  });

  const computePipeline = device.createComputePipeline({
    layout: 'auto',
    compute: { module: computeModule, entryPoint: 'main' },
  });

  const blitPipeline = device.createRenderPipeline({
    layout: 'auto',
    vertex: { module: blitModule, entryPoint: 'vs' },
    fragment: { module: blitModule, entryPoint: 'fs', targets: [{ format }] },
    primitive: { topology: 'triangle-list' },
  });

  const computeBind = device.createBindGroup({
    layout: computePipeline.getBindGroupLayout(0),
    entries: [
      { binding: 0, resource: { buffer: uniformBuf } },
      { binding: 1, resource: texture.createView() },
      { binding: 2, resource: { buffer: countsBuf } },
    ],
  });

  const blitBind = device.createBindGroup({
    layout: blitPipeline.getBindGroupLayout(0),
    entries: [
      { binding: 0, resource: texture.createView() },
      { binding: 1, resource: device.createSampler({ magFilter: 'linear', minFilter: 'linear' }) },
    ],
  });

  const scratch = new ArrayBuffer(64);
  const f32 = new Float32Array(scratch);
  const i32 = new Int32Array(scratch);
  const u32 = new Uint32Array(scratch);
  const zeros = new Uint32Array([0, 0]);

  let busy = false;

  async function render() {
    if (busy) return;
    busy = true;
    const started = performance.now();

    const worldSeed = readSeed();
    const seedContinent = featureSeed(worldSeed, FEATURE_CONTINENT);
    const seedDanger = featureSeed(worldSeed, FEATURE_DANGER);

    f32[0] = WORLD.radius;
    f32[1] = WORLD.continentFreq;
    f32[2] = WORLD.centreBias;
    f32[3] = WORLD.edgeDrop;
    f32[4] = WORLD.landOffset;
    f32[5] = WORLD.dangerW;
    f32[6] = WORLD.dangerFreq;
    f32[7] = WORLD.seaLevel;
    i32[8] = WORLD.continentOctaves;
    i32[9] = WORLD.dangerOctaves;
    i32[10] = Number(($('layer') as HTMLSelectElement).value);
    i32[11] = RES;
    u32[12] = seedContinent;
    u32[13] = seedDanger;
    u32[14] = 0;
    u32[15] = 0;

    device.queue.writeBuffer(uniformBuf, 0, scratch);
    device.queue.writeBuffer(countsBuf, 0, zeros);

    const enc = device.createCommandEncoder();
    const pass = enc.beginComputePass();
    pass.setPipeline(computePipeline);
    pass.setBindGroup(0, computeBind);
    const groups = Math.ceil(RES / 8);
    pass.dispatchWorkgroups(groups, groups);
    pass.end();

    const draw = enc.beginRenderPass({
      colorAttachments: [
        {
          view: ctx!.getCurrentTexture().createView(),
          loadOp: 'clear',
          storeOp: 'store',
          clearValue: { r: 0, g: 0, b: 0, a: 1 },
        },
      ],
    });
    draw.setPipeline(blitPipeline);
    draw.setBindGroup(0, blitBind);
    draw.draw(3);
    draw.end();

    enc.copyBufferToBuffer(countsBuf, 0, readbackBuf, 0, 8);
    device.queue.submit([enc.finish()]);

    await device.queue.onSubmittedWorkDone();
    const elapsed = performance.now() - started;

    await readbackBuf.mapAsync(GPUMapMode.READ);
    const counts = new Uint32Array(readbackBuf.getMappedRange().slice(0));
    readbackBuf.unmap();

    const land = counts[0];
    const disc = counts[1];
    $('land').textContent = disc > 0 ? `${((100 * land) / disc).toFixed(2)}%` : '–';
    $('ms').textContent = `${elapsed.toFixed(1)} ms`;
    $('radius').textContent = `${(WORLD.radius / 1000).toFixed(0)} km`;
    $('sc').textContent = `0x${seedContinent.toString(16).padStart(8, '0')}`;
    $('sd').textContent = `0x${seedDanger.toString(16).padStart(8, '0')}`;

    busy = false;
  }

  function readSeed(): bigint {
    const raw = ($('seed') as HTMLInputElement).value.trim();
    try {
      return BigInt(raw === '' ? '0' : raw) & MASK64;
    } catch {
      return 0n;
    }
  }

  function stepSeed(delta: bigint) {
    const input = $('seed') as HTMLInputElement;
    const next = (readSeed() + delta) & MASK64;
    input.value = next.toString();
    void render();
  }

  $('seed').addEventListener('change', () => void render());
  $('prev').addEventListener('click', () => stepSeed(-1n));
  $('next').addEventListener('click', () => stepSeed(1n));
  $('layer').addEventListener('change', () => void render());

  await render();
}

void main();
